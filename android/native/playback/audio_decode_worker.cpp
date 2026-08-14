#include "audio_decode_worker.h"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "mediacodec/mediacodec_video_decoder.h"
#include "playback_constants.h"
#include "streambridge/logging.h"

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeAudioWorker";

}  // namespace

AudioDecodeWorker::AudioDecodeWorker(
        streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue,
        std::atomic<int64_t>& first_audio_pts_us,
        streambridge::MediaClock& clock,
        PlaybackMetrics& metrics,
        Callbacks callbacks)
    : audio_queue_(audio_queue)
    , first_audio_pts_us_(first_audio_pts_us)
    , clock_(clock)
    , metrics_(metrics)
    , callbacks_(std::move(callbacks)) {}

void AudioDecodeWorker::run() {
    streambridge::StreamInfo audio_info_copy;
    if (!wait_for_audio_info(audio_info_copy)) {
        return;
    }

    audio_decoder_ = streambridge::android::mediacodec::create_audio_decoder();
    if (!audio_decoder_) {
        if (callbacks_.on_error) callbacks_.on_error("failed to create audio decoder");
        return;
    }

    auto open_result = audio_decoder_->open(audio_info_copy);
    if (open_result.is_err()) {
        if (callbacks_.on_error) callbacks_.on_error(open_result.error_message());
        return;
    }

    auto audio_open = audio_output_.open(audio_info_copy.sample_rate, audio_info_copy.channels);
    if (audio_open.is_err()) {
        if (callbacks_.on_error) callbacks_.on_error(audio_open.error_message());
        return;
    }
    SB_LOG_I(kLogTag, "audio decoder and AAudio output opened");

    int64_t local_first_audio_pts = first_audio_pts_us_;

    while (!callbacks_.is_stopping()) {
        streambridge::MediaPacket packet;
        auto pop_result = audio_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) break;
        if (pop_result == streambridge::QueueResult::Timeout) continue;

        auto send_result = audio_decoder_->send_packet(packet);
        if (send_result.is_err()) {
            if (callbacks_.on_error) callbacks_.on_error(send_result.error_message());
            return;
        }

        while (!callbacks_.is_stopping()) {
            auto dec_result = audio_decoder_->receive_frame();
            if (dec_result.is_err()) {
                if (callbacks_.on_error) callbacks_.on_error(dec_result.error_message());
                return;
            }
            if (!dec_result->has_frame) break;

            auto& frame = dec_result->frame;
            if (local_first_audio_pts >= 0 && frame.pts.us > 0) {
                frame.pts.us -= local_first_audio_pts;
            }

            audio_output_.start();

            auto write_result = audio_output_.write(frame);
            if (write_result.is_err()) {
                SB_LOG_W(kLogTag, "audio write failed: %s",
                         write_result.error_message().c_str());
            } else {
                metrics_.on_audio_frame_output();
            }

            clock_.update_audio(
                streambridge::TimePointUs{0},
                audio_output_.played_frames(),
                audio_info_copy.sample_rate);
        }
    }

    close();
    SB_LOG_I(kLogTag, "audio thread exiting");
}

void AudioDecodeWorker::close() {
    if (audio_decoder_) {
        audio_decoder_->drain();
        audio_decoder_->close();
        audio_decoder_.reset();
    }
    audio_output_.close();
}

bool AudioDecodeWorker::wait_for_audio_info(streambridge::StreamInfo& info) {
    int waits = 0;
    while (!callbacks_.is_stopping()) {
        if (callbacks_.copy_audio_info && callbacks_.copy_audio_info(info)) {
            return true;
        }
        if (callbacks_.state &&
                callbacks_.state() == streambridge::SessionState::Error) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (++waits > 200) break;
    }

    SB_LOG_I(kLogTag, "no audio stream, audio thread exiting");
    return false;
}

}  // namespace streambridge::android
