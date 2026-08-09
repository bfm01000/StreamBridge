#pragma once

#include <android/native_window.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "streambridge/av_sync.h"
#include "streambridge/media_queue.h"
#include "streambridge/session.h"

#include "ffmpeg/ffmpeg_audio_decoder.h"
#include "ffmpeg/ffmpeg_subscriber.h"
#include "ffmpeg/ffmpeg_video_decoder.h"
#include "native_audio_output.h"
#include "native_video_renderer.h"

namespace streambridge::android {

class NativePlaybackSession {
public:
    NativePlaybackSession();
    ~NativePlaybackSession();

    NativePlaybackSession(const NativePlaybackSession&) = delete;
    NativePlaybackSession& operator=(const NativePlaybackSession&) = delete;

    int start(std::string url, ANativeWindow* window);
    void stop();

    void set_surface(ANativeWindow* window);
    void clear_surface();

    streambridge::SessionState state() const;
    std::string status_text() const;

private:
    // Thread entry points
    void demux_loop();
    void video_loop();
    void audio_loop();

    // State management
    void set_state(streambridge::SessionState s);
    void set_state_locked(streambridge::SessionState s);
    void set_error(const std::string& msg);
    void request_stop();
    bool is_stopping() const;

    // State (mutex protected)
    mutable std::mutex mutex_;
    streambridge::SessionState state_ = streambridge::SessionState::Idle;
    std::string last_error_;
    std::string url_;

    // Sync primitives
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> surface_paused_{false};     // surface destroyed -> pause rendering
    std::atomic<bool> stop_in_progress_{false};   // guard against double-stop

    // Components
    ffmpeg::FFmpegSubscriber subscriber_;
    ffmpeg::FFmpegVideoDecoder video_decoder_;
    ffmpeg::FFmpegAudioDecoder audio_decoder_;
    NativeVideoRenderer renderer_;
    NativeAudioOutput audio_output_;
    streambridge::MediaClock clock_;
    streambridge::AVSyncController sync_controller_;

    // Queues
    streambridge::MediaQueue<streambridge::MediaPacket> video_packet_queue_;
    streambridge::MediaQueue<streambridge::MediaPacket> audio_packet_queue_;

    // Threads
    std::thread demux_thread_;
    std::thread video_thread_;
    std::thread audio_thread_;

    // Metrics (for status_text)
    int64_t video_frames_rendered_ = 0;
    int64_t video_frames_dropped_ = 0;
    int64_t audio_frames_output_ = 0;
    int64_t last_av_diff_us_ = 0;
    streambridge::VideoSyncAction last_sync_action_ = streambridge::VideoSyncAction::Render;

    // First PTS for normalization (demux writes, decode reads)
    std::atomic<int64_t> first_video_pts_us_{-1};
    std::atomic<int64_t> first_audio_pts_us_{-1};

    // Stream info (demux writes, decode reads)
    const streambridge::StreamInfo* video_info_ = nullptr;
    const streambridge::StreamInfo* audio_info_ = nullptr;
};

}  // namespace streambridge::android
