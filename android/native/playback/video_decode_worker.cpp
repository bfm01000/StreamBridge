#include "video_decode_worker.h"

#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "ffmpeg/ffmpeg_video_decoder.h"
#include "mediacodec/mediacodec_video_decoder.h"
#include "playback_constants.h"
#include "streambridge/logging.h"

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeVideoWorker";

}  // namespace

VideoDecodeWorker::VideoDecodeWorker(
        streambridge::MediaQueue<streambridge::MediaPacket>& video_queue,
        streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue,
        std::atomic<int64_t>& first_video_pts_us,
        streambridge::MediaClock& clock,
        streambridge::AVSyncController& sync_controller,
        NativeVideoRenderer& renderer,
        PlaybackMetrics& metrics,
        VideoDecodePath decode_path,
        Callbacks callbacks)
    : video_queue_(video_queue)
    , audio_queue_(audio_queue)
    , first_video_pts_us_(first_video_pts_us)
    , clock_(clock)
    , sync_controller_(sync_controller)
    , renderer_(renderer)
    , metrics_(metrics)
    , decode_path_(decode_path)
    , callbacks_(std::move(callbacks)) {}

void VideoDecodeWorker::run() {
    SB_LOG_I(kLogTag, "video thread started");

    streambridge::StreamInfo video_info;
    if (!wait_for_video_info(video_info)) {
        return;
    }
    if (callbacks_.is_stopping()) {
        return;
    }

    if (!open_decoder(video_info)) {
        return;
    }

    decode_loop();
    close();
    SB_LOG_I(kLogTag, "video thread exiting");
}

void VideoDecodeWorker::close() {
    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
    if (video_decoder_) {
        video_decoder_->drain();
        video_decoder_->close();
        video_decoder_.reset();
    }
}

streambridge::Result<void> VideoDecodeWorker::set_surface(ANativeWindow* window) {
    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
    if (!video_decoder_) {
        return streambridge::Result<void>::ok();
    }
    return streambridge::android::mediacodec::set_decoder_surface(video_decoder_.get(), window);
}

bool VideoDecodeWorker::wait_for_video_info(streambridge::StreamInfo& info) {
    int waits = 0;
    while (!callbacks_.is_stopping()) {
        if (callbacks_.copy_video_info && callbacks_.copy_video_info(info)) {
            return true;
        }
        if (callbacks_.state &&
                callbacks_.state() == streambridge::SessionState::Error) {
            return false;
        }
        if (callbacks_.state) {
            const auto state = callbacks_.state();
            if (state == streambridge::SessionState::Reconnecting ||
                    state == streambridge::SessionState::Paused) {
                waits = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (++waits > 200) {
            if (callbacks_.on_error) {
                callbacks_.on_error("timeout waiting for video stream info");
            }
            return false;
        }
    }
    return false;
}

bool VideoDecodeWorker::open_decoder(const streambridge::StreamInfo& info) {
    SB_LOG_I(kLogTag,
            "video: opening decoder codec=%d %dx%d",
            static_cast<int>(info.codec), info.width, info.height);
    metrics_.set_video_path(video_decode_path_name(decode_path_));

    {
        std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
        video_decoder_ = streambridge::android::mediacodec::create_video_decoder(
            renderer_.window(), decode_path_);
    }
    if (!video_decoder_) {
        if (callbacks_.on_error) callbacks_.on_error("failed to create video decoder");
        return false;
    }

    auto open_result = video_decoder_->open(info);
    if (open_result.is_err()) {
        SB_LOG_W(kLogTag, "video: hardware decoder open failed, fallback to software: %s",
                 open_result.error_message().c_str());
        video_decoder_ = std::make_unique<streambridge::ffmpeg::FFmpegVideoDecoder>();
        open_result = video_decoder_->open(info);
        if (open_result.is_err()) {
            if (callbacks_.on_error) callbacks_.on_error(open_result.error_message());
            return false;
        }
    }
    SB_LOG_I(kLogTag, "video decoder opened, mode=%s",
             video_decoder_->capability().supports_surface_output ?
                "MediaCodec zero-copy" : "FFmpeg software");
    if (!video_decoder_->capability().hardware) {
        metrics_.set_video_path("FFMPEG_SOFTWARE");
    }
    return true;
}

void VideoDecodeWorker::decode_loop() {
    int pkt_fed = 0;
    int frame_out = 0;
    int pkt_drop = 0;
    auto last_heartbeat = std::chrono::steady_clock::now();

    while (!callbacks_.is_stopping()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat > std::chrono::seconds(2)) {
            last_heartbeat = now;
            SB_LOG_I(kLogTag,
                "video: HEARTBEAT pkt_fed=%d frame_out=%d dropped=%d vq=%zu aq=%zu "
                "has_audio=%d sync=%s",
                pkt_fed, frame_out, pkt_drop,
                video_queue_.size(), audio_queue_.size(),
                clock_.has_audio_clock() ? 1 : 0,
                streambridge::video_sync_action_name(metrics_.last_sync_action()));
        }

        streambridge::MediaPacket packet;
        auto pop_result = video_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) {
            SB_LOG_I(kLogTag, "video: pop ABORTED, exiting");
            break;
        }
        if (pop_result == streambridge::QueueResult::Timeout) {
            continue;
        }

        ++pkt_fed;
        metrics_.on_video_packet_fed();

        streambridge::Result<streambridge::DecodeStatus> send_result =
            streambridge::Result<streambridge::DecodeStatus>::ok(
                streambridge::DecodeStatus::TryAgain);
        {
            std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
            send_result = video_decoder_->send_packet(packet);
        }
        if (send_result.is_err()) {
            if (callbacks_.on_error) callbacks_.on_error(send_result.error_message());
            return;
        }
        if (pkt_fed <= 10 || pkt_fed % 50 == 0) {
            SB_LOG_I(kLogTag, "video: fed pkt#%d pts=%lld size=%zu q=%zu",
                     pkt_fed, static_cast<long long>(packet.pts.us),
                     packet.data.size(), video_queue_.size());
        }

        if (!receive_ready_frames(frame_out, pkt_drop)) {
            return;
        }
    }
}

bool VideoDecodeWorker::receive_ready_frames(int& frame_out, int& pkt_drop) {
    while (!callbacks_.is_stopping()) {
        streambridge::Result<streambridge::DecodeOutput> recv =
            streambridge::Result<streambridge::DecodeOutput>::err(
                streambridge::ErrorDomain::Internal,
                streambridge::ErrorCode::QueueTimeout,
                "not ready");
        {
            std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
            recv = video_decoder_->receive_frame(kVideoDrainTimeoutMs);
        }
        if (recv.is_err()) {
            if (recv.error_code() == streambridge::ErrorCode::QueueTimeout) {
                break;
            }
            if (callbacks_.on_error) callbacks_.on_error(recv.error_message());
            return false;
        }

        ++frame_out;
        metrics_.on_video_frame_decoded();
        if (!handle_output(*recv, pkt_drop)) {
            return false;
        }
    }
    return true;
}

bool VideoDecodeWorker::handle_output(streambridge::DecodeOutput& out, int& pkt_drop) {
    const int64_t norm_pts_us = normalize_video_pts_us(out.pts_us);
    auto sync = decide_video_sync(norm_pts_us);
    log_decode_decision(norm_pts_us, sync);

    const FrameFlow sync_flow =
        apply_sync_decision(out, norm_pts_us, sync, pkt_drop);
    if (sync_flow == FrameFlow::FrameHandled) {
        return true;
    }
    if (sync_flow == FrameFlow::Stop) {
        return false;
    }

    if (!wait_until_surface_ready()) {
        return false;
    }
    if (!render_output(out)) {
        return false;
    }

    metrics_.on_video_frame_presented();
    log_presented_frame(norm_pts_us, sync);
    log_stability(norm_pts_us, sync);
    return true;
}

int64_t VideoDecodeWorker::normalize_video_pts_us(int64_t pts_us) const {
    return (first_video_pts_us_ >= 0 && pts_us > 0)
        ? pts_us - first_video_pts_us_
        : pts_us;
}

VideoDecodeWorker::SyncState VideoDecodeWorker::decide_video_sync(int64_t norm_pts_us) {
    SyncState sync;
    sync.enabled = clock_.has_audio_clock();
    if (!sync.enabled) {
        return sync;
    }

    sync.decision = sync_controller_.decide(
        streambridge::TimePointUs{norm_pts_us},
        clock_.now());
    metrics_.set_sync(sync.decision.action, sync.decision.av_diff_us);
    return sync;
}

VideoDecodeWorker::FrameFlow VideoDecodeWorker::apply_sync_decision(
        streambridge::DecodeOutput& out,
        int64_t norm_pts_us,
        SyncState& sync,
        int& pkt_drop) {
    if (!sync.enabled) {
        return FrameFlow::ContinueRender;
    }

    if (sync.decision.action == streambridge::VideoSyncAction::Drop) {
        ++pkt_drop;
        metrics_.on_video_frame_dropped();
        return discard_output(out) ? FrameFlow::FrameHandled : FrameFlow::Stop;
    }

    if (sync.decision.action != streambridge::VideoSyncAction::Wait) {
        return FrameFlow::ContinueRender;
    }

    std::this_thread::sleep_for(std::chrono::microseconds(sync.decision.wait_us));
    sync.decision = sync_controller_.decide(
        streambridge::TimePointUs{norm_pts_us},
        clock_.now());

    if (sync.decision.action == streambridge::VideoSyncAction::Drop) {
        ++pkt_drop;
        metrics_.on_video_frame_dropped();
        return discard_output(out) ? FrameFlow::FrameHandled : FrameFlow::Stop;
    }
    return FrameFlow::ContinueRender;
}

bool VideoDecodeWorker::wait_until_surface_ready() {
    while (callbacks_.is_surface_paused && callbacks_.is_surface_paused() &&
            !callbacks_.is_stopping()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !callbacks_.is_stopping();
}

bool VideoDecodeWorker::render_output(streambridge::DecodeOutput& out) {
    bool frame_ok = true;
    std::visit([&](auto&& handle) {
        using T = std::decay_t<decltype(handle)>;
        if constexpr (std::is_same_v<T, streambridge::CpuFrameHandle>) {
            frame_ok = render_cpu_frame(handle) && discard_output(out);
        } else if constexpr (std::is_same_v<T, streambridge::DecoderSurfaceHandle>) {
            (void)handle;
            frame_ok = render_surface_frame(out);
        } else if constexpr (std::is_same_v<T, streambridge::HardwareBufferFrameHandle>) {
            frame_ok = render_hardware_buffer_frame(handle) && discard_output(out);
        }
    }, out.payload);
    return frame_ok;
}

bool VideoDecodeWorker::render_cpu_frame(const streambridge::CpuFrameHandle& handle) {
    if (!handle.frame.is_valid()) {
        return true;
    }

    static int rc = 0;
    if (++rc <= 3) {
        SB_LOG_I(kLogTag,
            "render cpu %dx%d data=%p buf=%p stride=%d",
            handle.frame.width,
            handle.frame.height,
            static_cast<void*>(handle.frame.planes[0].data),
            static_cast<void*>(handle.buffer ? handle.buffer->data() : nullptr),
            handle.frame.planes[0].stride);
    }
    renderer_.render(handle.frame);
    metrics_.set_render_path("CPU");
    return true;
}

bool VideoDecodeWorker::render_surface_frame(streambridge::DecodeOutput& out) {
    metrics_.set_render_path("SURFACE");
    return present_output(out, 0);
}

bool VideoDecodeWorker::render_hardware_buffer_frame(
        const streambridge::HardwareBufferFrameHandle& handle) {
    if (!handle.frame.is_valid()) {
        SB_LOG_W(kLogTag,
                 "hardware buffer frame is invalid; dropping frame");
        return true;
    }

    auto render_result = renderer_.render(handle.frame);
    if (render_result.is_err()) {
        SB_LOG_W(kLogTag, "render hardware buffer failed: %s",
                 render_result.error_message().c_str());
        metrics_.set_render_path("AHB_FAILED");
        return true;
    }

    metrics_.set_render_path("AHB_GPU");
    return true;
}

void VideoDecodeWorker::log_decode_decision(int64_t norm_pts_us, const SyncState& sync) {
    const int64_t decoded = metrics_.video_frames_decoded();
    if (decoded > 10 && decoded % 50 != 0) {
        return;
    }

    SB_LOG_I(kLogTag, "video: frame#%lld pts=%lld av=%lld act=%s",
             static_cast<long long>(decoded),
             static_cast<long long>(norm_pts_us),
             static_cast<long long>(sync.decision.av_diff_us),
             streambridge::video_sync_action_name(
                sync.enabled ? sync.decision.action : streambridge::VideoSyncAction::Render));
}

void VideoDecodeWorker::log_presented_frame(int64_t norm_pts_us, const SyncState& sync) {
    const int64_t presented = metrics_.video_frames_presented();
    if (presented > 0 && presented % 100 == 0) {
        SB_LOG_I(kLogTag,
                "video: frame=%lld pts=%lld av=%lld q=%zu",
                static_cast<long long>(presented),
                static_cast<long long>(norm_pts_us),
                static_cast<long long>(sync.decision.av_diff_us),
                video_queue_.size());
    }
}

void VideoDecodeWorker::log_stability(int64_t norm_pts_us, const SyncState& sync) {
    static int64_t last_log_pts_us = 0;
    if (norm_pts_us - last_log_pts_us >= 5'000'000) {
        last_log_pts_us = norm_pts_us;
        SB_LOG_I(kLogTag,
                "stability: pts=%lld rendered=%lld dropped=%lld "
                "vq=%zu aq=%zu av=%lld state=%s",
                static_cast<long long>(norm_pts_us),
                static_cast<long long>(metrics_.video_frames_presented()),
                static_cast<long long>(metrics_.video_frames_dropped()),
                video_queue_.size(),
                audio_queue_.size(),
                static_cast<long long>(sync.decision.av_diff_us),
                callbacks_.state ?
                    streambridge::session_state_name(callbacks_.state()) : "?");
    }
}

bool VideoDecodeWorker::discard_output(streambridge::DecodeOutput& out) {
    if (!out.lease) {
        return true;
    }

    auto result = out.lease->discard();
    out.lease.reset();
    if (result.is_err()) {
        SB_LOG_W(kLogTag, "discard frame failed: %s", result.error_message().c_str());
        return false;
    }
    return true;
}

bool VideoDecodeWorker::present_output(
        streambridge::DecodeOutput& out,
        int64_t target_time_ns) {
    if (!out.lease) {
        SB_LOG_W(kLogTag,
                 "surface frame_id=%llu has no release lease",
                 static_cast<unsigned long long>(out.frame_id));
        return false;
    }

    auto result = out.lease->present(target_time_ns);
    out.lease.reset();
    if (result.is_err()) {
        SB_LOG_W(kLogTag, "present frame failed: %s", result.error_message().c_str());
        return false;
    }
    return true;
}

}  // namespace streambridge::android
