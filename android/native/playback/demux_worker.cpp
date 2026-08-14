#include "demux_worker.h"

#include <chrono>
#include <thread>
#include <utility>

#include "playback_constants.h"
#include "playback_reconnect_controller.h"
#include "streambridge/logging.h"

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeDemux";

}  // namespace

DemuxWorker::DemuxWorker(
        streambridge::MediaQueue<streambridge::MediaPacket>& video_queue,
        streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue,
        std::atomic<int64_t>& first_video_pts_us,
        std::atomic<int64_t>& first_audio_pts_us,
        streambridge::MediaClock& clock,
        PlaybackMetrics& metrics,
        Callbacks callbacks)
    : video_queue_(video_queue)
    , audio_queue_(audio_queue)
    , first_video_pts_us_(first_video_pts_us)
    , first_audio_pts_us_(first_audio_pts_us)
    , clock_(clock)
    , metrics_(metrics)
    , callbacks_(std::move(callbacks)) {}

void DemuxWorker::run(const std::string& url) {
    SB_LOG_I(kLogTag, "demux thread started");

    PlaybackReconnectController reconnect(5, 2000);

    while (!is_stopping() && reconnect.can_try()) {
        if (reconnect.is_reconnecting()) {
            SB_LOG_I(kLogTag,
                    "reconnect attempt %d/%d after %dms",
                    reconnect.attempt(), reconnect.max_attempts(), reconnect.delay_ms());

            video_queue_.flush();
            audio_queue_.flush();
            first_video_pts_us_ = -1;
            first_audio_pts_us_ = -1;
            clock_.reset();

            if (callbacks_.on_reconnecting) {
                callbacks_.on_reconnecting();
            }

            for (int i = 0; i < reconnect.delay_ms() / 100 && !is_stopping(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (is_stopping()) break;
        }

        auto open_result = subscriber_.open(url);
        if (open_result.is_err()) {
            SB_LOG_W(kLogTag,
                    "open failed (attempt %d): %s",
                    reconnect.attempt(), open_result.error_message().c_str());
            reconnect.record_failure();
            continue;
        }

        if (callbacks_.on_stream_info) {
            callbacks_.on_stream_info(subscriber_.video_stream(), subscriber_.audio_stream());
        }
        if (callbacks_.on_running) {
            callbacks_.on_running();
        }

        clock_.start(streambridge::TimePointUs{0});
        reconnect.reset_after_connected();
        SB_LOG_I(kLogTag, "connected, reading packets");

        int packet_count = 0;
        bool connection_lost = false;
        while (!is_stopping()) {
            auto result = subscriber_.read_packet();
            if (result.is_err()) {
                SB_LOG_W(kLogTag, "read error: %s", result.error_message().c_str());
                connection_lost = true;
                break;
            }

            if (++packet_count == 1) {
                SB_LOG_I(kLogTag,
                        "first packet read, type=%d",
                        static_cast<int>(result->type));
            }

            if (result->type == streambridge::MediaType::Unknown) {
                SB_LOG_I(kLogTag, "EOF reached");
                connection_lost = true;
                break;
            }

            if (!push_packet(std::move(*result), connection_lost)) {
                break;
            }
        }

        subscriber_.close();
        if (!connection_lost) break;
        reconnect.record_failure();
    }

    video_queue_.abort();
    audio_queue_.abort();

    if (reconnect.exhausted() && callbacks_.on_error) {
        callbacks_.on_error("demux: max reconnect attempts exceeded");
    }

    SB_LOG_I(kLogTag, "demux thread exiting");
}

void DemuxWorker::close() {
    subscriber_.close();
}

bool DemuxWorker::is_stopping() const {
    return callbacks_.is_stopping && callbacks_.is_stopping();
}

bool DemuxWorker::push_packet(streambridge::MediaPacket packet, bool& connection_lost) {
    metrics_.on_packet_demuxed(packet);

    if (packet.type == streambridge::MediaType::Video) {
        if (first_video_pts_us_ < 0 && packet.has_valid_pts()) {
            first_video_pts_us_ = packet.pts.us;
        }
        auto push_result = video_queue_.push(
            std::move(packet),
            streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs));
        if (push_result == streambridge::QueueResult::Aborted) {
            connection_lost = false;
            return false;
        }
        if (push_result == streambridge::QueueResult::Timeout) {
            SB_LOG_W(kLogTag, "video packet queue blocked for %lldms, reconnect",
                     static_cast<long long>(kDemuxReadTimeoutMs));
            connection_lost = true;
            return false;
        }
        return true;
    }

    if (packet.type == streambridge::MediaType::Audio) {
        if (first_audio_pts_us_ < 0 && packet.has_valid_pts()) {
            first_audio_pts_us_ = packet.pts.us;
        }
        auto push_result = audio_queue_.push(
            std::move(packet),
            streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs));
        if (push_result == streambridge::QueueResult::Aborted) {
            connection_lost = false;
            return false;
        }
        if (push_result == streambridge::QueueResult::Timeout) {
            SB_LOG_W(kLogTag, "audio packet queue blocked for %lldms, reconnect",
                     static_cast<long long>(kDemuxReadTimeoutMs));
            connection_lost = true;
            return false;
        }
    }

    return true;
}

}  // namespace streambridge::android
