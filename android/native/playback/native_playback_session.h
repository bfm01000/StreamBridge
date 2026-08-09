#pragma once

#include <android/native_window.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "streambridge/media_queue.h"
#include "streambridge/session.h"

#include "ffmpeg/ffmpeg_audio_decoder.h"
#include "ffmpeg/ffmpeg_subscriber.h"
#include "ffmpeg/ffmpeg_video_decoder.h"
#include "native_audio_output.h"
#include "native_video_renderer.h"
#include "playback_clock.h"

namespace streambridge::android {

class NativePlaybackSession {
public:
    NativePlaybackSession();
    ~NativePlaybackSession();

    NativePlaybackSession(const NativePlaybackSession&) = delete;
    NativePlaybackSession& operator=(const NativePlaybackSession&) = delete;

    // 启动播放（异步）：验证参数后启动后台线程
    // 调用后可通过 state() 轮询状态变化
    int start(std::string url, ANativeWindow* window);

    // 停止播放：通知所有线程退出，等待 join，清理资源
    void stop();

    // Surface 生命周期
    void set_surface(ANativeWindow* window);
    void clear_surface();

    // 状态查询（线程安全）
    streambridge::SessionState state() const;
    std::string status_text() const;

private:
    // ---- 线程入口 ----
    void demux_loop();
    void video_loop();
    void audio_loop();

    // ---- 状态管理 ----
    void set_state(streambridge::SessionState s);
    void set_error(const std::string& msg);
    void request_stop();
    bool is_stopping() const;

    // ---- 状态（mutex 保护）----
    mutable std::mutex mutex_;
    streambridge::SessionState state_ = streambridge::SessionState::Idle;
    std::string last_error_;
    std::string url_;

    // ---- 同步基元 ----
    std::atomic<bool> abort_requested_{false};

    // ---- 组件 ----
    ffmpeg::FFmpegSubscriber subscriber_;
    ffmpeg::FFmpegVideoDecoder video_decoder_;
    ffmpeg::FFmpegAudioDecoder audio_decoder_;
    NativeVideoRenderer renderer_;
    NativeAudioOutput audio_output_;
    PlaybackClock clock_;
    AVSyncController sync_controller_;

    // ---- 队列 ----
    streambridge::MediaQueue<streambridge::MediaPacket> video_packet_queue_;
    streambridge::MediaQueue<streambridge::MediaPacket> audio_packet_queue_;

    // ---- 线程 ----
    std::thread demux_thread_;
    std::thread video_thread_;
    std::thread audio_thread_;

    // ---- 指标（供 status_text 读取）----
    int64_t video_frames_rendered_ = 0;
    int64_t video_frames_dropped_ = 0;
    int64_t audio_frames_output_ = 0;
    int64_t last_av_diff_us_ = 0;
    VideoSyncAction last_sync_action_ = VideoSyncAction::Render;

    // 首帧 PTS（用于归一化，demux 线程写入，decode 线程读取）
    std::atomic<int64_t> first_video_pts_us_{-1};
    std::atomic<int64_t> first_audio_pts_us_{-1};

    // 流信息（demux 线程写入，decode 线程读取）
    const streambridge::StreamInfo* video_info_ = nullptr;
    const streambridge::StreamInfo* audio_info_ = nullptr;
};

}  // namespace streambridge::android
