#pragma once

#include <android/native_window.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "native_video_renderer.h"
#include "playback_metrics.h"
#include "streambridge/av_sync.h"
#include "streambridge/codec.h"
#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"
#include "streambridge/session.h"
#include "video_path_config.h"

namespace streambridge::android {

class VideoDecodeWorker {
public:
    struct Callbacks {
        std::function<bool()> is_stopping;
        std::function<streambridge::SessionState()> state;
        std::function<bool(streambridge::StreamInfo&)> copy_video_info;
        std::function<bool()> is_surface_paused;
        std::function<void(const std::string&)> on_error;
    };

    VideoDecodeWorker(streambridge::MediaQueue<streambridge::MediaPacket>& video_queue,
                      streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue,
                      std::atomic<int64_t>& first_video_pts_us,
                      streambridge::MediaClock& clock,
                      streambridge::AVSyncController& sync_controller,
                      NativeVideoRenderer& renderer,
                      PlaybackMetrics& metrics,
                      VideoDecodePath decode_path,
                      Callbacks callbacks);

    void run();
    void close();
    streambridge::Result<void> set_surface(ANativeWindow* window);

private:
    struct SyncState {
        bool enabled = false;
        streambridge::VideoSyncDecision decision;
    };

    enum class FrameFlow {
        ContinueRender,
        FrameHandled,
        Stop,
    };

    bool wait_for_video_info(streambridge::StreamInfo& info);
    bool open_decoder(const streambridge::StreamInfo& info);
    void decode_loop();
    bool receive_ready_frames(int& frame_out, int& pkt_drop);
    bool handle_output(streambridge::DecodeOutput& out, int& pkt_drop);
    int64_t normalize_video_pts_us(int64_t pts_us) const;
    SyncState decide_video_sync(int64_t norm_pts_us);
    FrameFlow apply_sync_decision(
        streambridge::DecodeOutput& out,
        int64_t norm_pts_us,
        SyncState& sync,
        int& pkt_drop);
    bool wait_until_surface_ready();
    bool render_output(streambridge::DecodeOutput& out);
    bool render_cpu_frame(const streambridge::CpuFrameHandle& handle);
    bool render_surface_frame(streambridge::DecodeOutput& out);
    bool render_hardware_buffer_frame(const streambridge::HardwareBufferFrameHandle& handle);
    void log_decode_decision(int64_t norm_pts_us, const SyncState& sync);
    void log_presented_frame(int64_t norm_pts_us, const SyncState& sync);
    void log_stability(int64_t norm_pts_us, const SyncState& sync);
    bool discard_output(streambridge::DecodeOutput& out);
    bool present_output(streambridge::DecodeOutput& out, int64_t target_time_ns);

    streambridge::MediaQueue<streambridge::MediaPacket>& video_queue_;
    streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue_;
    std::atomic<int64_t>& first_video_pts_us_;
    streambridge::MediaClock& clock_;
    streambridge::AVSyncController& sync_controller_;
    NativeVideoRenderer& renderer_;
    PlaybackMetrics& metrics_;
    VideoDecodePath decode_path_ = VideoDecodePath::Auto;
    Callbacks callbacks_;

    mutable std::mutex decoder_mutex_;
    std::unique_ptr<streambridge::IVideoDecoder> video_decoder_;
};

}  // namespace streambridge::android
