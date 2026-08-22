#pragma once
// 会话定义 — PublishSession 和 PlaybackSession

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "streambridge/capture.h"
#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"
#include "streambridge/stop_token.h"
#include "streambridge/transport.h"
#include "streambridge/transport_config.h"

namespace streambridge {

// ============================================================
// 会话状态
// ============================================================
enum class SessionState {
    Idle,
    Preparing,
    Prepared,
    Running,       // 发布端：正在推流
    Paused,
    Reconnecting,
    Stopping,
    Stopped,
    Error,
};

const char* session_state_name(SessionState state);

// ============================================================
// 会话观察者
// ============================================================
// 会话观察者：回调状态迁移、错误与运行指标
class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;

    virtual void on_state_changed(SessionState old_state,
                                  SessionState new_state) = 0;
    virtual void on_error(ErrorDomain domain, ErrorCode code,
                          const std::string& message) = 0;

    struct Metrics {
        int64_t frames_captured = 0;
        int64_t frames_encoded = 0;
        int64_t packets_sent = 0;
        int64_t bytes_sent = 0;
        int64_t frames_dropped = 0;
        // 队列水位
        size_t raw_video_queue = 0;
        size_t raw_audio_queue = 0;
        size_t video_pkt_queue = 0;
        size_t audio_pkt_queue = 0;
    };
    virtual void on_metrics(const Metrics& metrics) = 0;
};

// ============================================================
// PublishSession 配置
// ============================================================
// 推流会话配置：采集/编码/发布参数与队列容量
struct PublishSessionConfig {
    VideoCaptureConfig video_capture;
    AudioCaptureConfig audio_capture;
    VideoEncodeConfig video_encode;
    AudioEncodeConfig audio_encode;
    TransportConfig transport;

    // 队列配置
    size_t raw_video_queue_size = 3;
    TimeDeltaUs raw_audio_queue_duration{200'000};
    TimeDeltaUs pkt_queue_duration{2'000'000};

    bool enable_audio = true;  // M2: video only = false
};

// ============================================================
// PublishSession
// ============================================================
// 推流会话：编排采集-编码-发布全链路，维护状态机与线程
class PublishSession {
public:
    PublishSession(std::unique_ptr<IVideoCapture> video_capture,
                   std::unique_ptr<IAudioCapture> audio_capture,
                   std::unique_ptr<IVideoEncoder> video_encoder,
                   std::unique_ptr<IAudioEncoder> audio_encoder,
                   std::unique_ptr<IMediaPublisher> publisher);

    ~PublishSession();

    // 禁止拷贝
    PublishSession(const PublishSession&) = delete;
    PublishSession& operator=(const PublishSession&) = delete;

    // === 生命周期 ===
    Result<void> prepare(const PublishSessionConfig& config);
    Result<void> start();
    void stop();
    void reset();

    // === 状态 ===
    SessionState state() const;

    // === 观测 ===
    void set_observer(std::shared_ptr<ISessionObserver> observer);
    ISessionObserver::Metrics metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================
// PlaybackSession 配置（M4 实现，此处仅声明）
// ============================================================
// 拉流播放会话配置（M4 实现，当前仅声明）
struct PlaybackSessionConfig {
    SubscribeConfig subscribe;
    VideoDecodeConfig video_decode;
    AudioDecodeConfig audio_decode;
    // ...
};

}  // namespace streambridge
