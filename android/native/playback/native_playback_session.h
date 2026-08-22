#pragma once

#include <android/native_window.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "streambridge/av_sync.h"
#include "streambridge/media_queue.h"
#include "streambridge/session.h"

#include "audio_decode_worker.h"
#include "android_rtp_udp_video_receiver.h"
#include "demux_worker.h"
#include "native_video_renderer.h"
#include "playback_metrics.h"
#include "video_path_config.h"
#include "video_decode_worker.h"

namespace streambridge::android {

// Android ????????????????????????????????????????????Session ???????????????
class NativePlaybackSession {
public:
    NativePlaybackSession();
    ~NativePlaybackSession();

    NativePlaybackSession(const NativePlaybackSession&) = delete;
    NativePlaybackSession& operator=(const NativePlaybackSession&) = delete;

    int start(std::string url, ANativeWindow* window,
              VideoDecodePath decode_path = VideoDecodePath::Auto);
    int start_rtp_video(uint16_t local_port, ANativeWindow* window,
                        int width, int height, double frame_rate,
                        VideoDecodePath decode_path = VideoDecodePath::Auto);
    void stop();

    void set_surface(ANativeWindow* window);
    void clear_surface();

    streambridge::SessionState state() const;
    std::string status_text() const;

private:
    // Thread entry points
    void demux_loop();
    void rtp_video_receive_loop();
    void video_loop();
    void audio_loop();

    // State management
    void set_state(streambridge::SessionState s);
    void set_state_locked(streambridge::SessionState s);
    void set_error(const std::string& msg);
    void request_stop();
    bool is_stopping() const;

    enum class InputMode { Rtmp, RtpUdpVideo };

    // State (mutex protected)
    mutable std::mutex mutex_;
    streambridge::SessionState state_ = streambridge::SessionState::Idle;
    std::string last_error_;
    std::string url_;
    InputMode input_mode_ = InputMode::Rtmp;
    uint16_t rtp_local_port_ = 0;
    int rtp_video_width_ = 0;
    int rtp_video_height_ = 0;
    double rtp_video_frame_rate_ = 0.0;
    VideoDecodePath decode_path_ = VideoDecodePath::Auto;

    // Sync primitives
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> surface_paused_{false};     // surface destroyed -> pause rendering
    std::atomic<bool> stop_in_progress_{false};   // guard against double-stop

    // Components
    std::unique_ptr<DemuxWorker> demux_worker_;
    std::unique_ptr<AndroidRtpUdpVideoReceiver> rtp_video_receiver_;
    std::unique_ptr<VideoDecodeWorker> video_worker_;
    std::unique_ptr<AudioDecodeWorker> audio_worker_;
    NativeVideoRenderer renderer_;
    streambridge::MediaClock clock_;
    streambridge::AVSyncController sync_controller_;

    // Queues
    streambridge::MediaQueue<streambridge::MediaPacket> video_packet_queue_;
    streambridge::MediaQueue<streambridge::MediaPacket> audio_packet_queue_;

    // Threads
    std::thread demux_thread_;
    std::thread video_thread_;
    std::thread audio_thread_;

    PlaybackMetrics metrics_;

    // First PTS for normalization (demux writes, decode reads)
    std::atomic<int64_t> first_video_pts_us_{-1};
    std::atomic<int64_t> first_audio_pts_us_{-1};

    // Stream info snapshots (demux writes, decode reads)
    std::optional<streambridge::StreamInfo> video_info_;
    std::optional<streambridge::StreamInfo> audio_info_;
};

}  // namespace streambridge::android
