// PublishSession 实现 — 线程管理、状态机、数据流编排
// M3: 支持视频+音频推流

#include "streambridge/session.h"
#include "logger.h"
#include <chrono>
#include <thread>

namespace streambridge {

const char* session_state_name(SessionState state) {
    switch (state) {
        case SessionState::Idle:      return "Idle";
        case SessionState::Preparing: return "Preparing";
        case SessionState::Prepared:  return "Prepared";
        case SessionState::Running:   return "Running";
        case SessionState::Paused:    return "Paused";
        case SessionState::Reconnecting: return "Reconnecting";
        case SessionState::Stopping:  return "Stopping";
        case SessionState::Stopped:   return "Stopped";
        case SessionState::Error:     return "Error";
        default: return "Unknown";
    }
}

// ============================================================
// PublishSession::Impl
// ============================================================
struct PublishSession::Impl {
    // 平台适配器（依赖注入）
    std::unique_ptr<IVideoCapture> video_capture;
    std::unique_ptr<IAudioCapture> audio_capture;
    std::unique_ptr<IVideoEncoder> video_encoder;
    std::unique_ptr<IAudioEncoder> audio_encoder;
    std::unique_ptr<IMediaPublisher> publisher;

    // 配置
    PublishSessionConfig config;
    bool has_audio = false;

    // 队列
    MediaQueue<VideoFrame> raw_video_queue;
    MediaQueue<AudioFrame> raw_audio_queue;
    MediaQueue<MediaPacket> video_pkt_queue;
    MediaQueue<MediaPacket> audio_pkt_queue;

    // 状态
    std::atomic<SessionState> state{SessionState::Idle};
    std::shared_ptr<ISessionObserver> observer;

    // 停止控制
    StopSource stop_source;

    // 线程（capture 线程由各自 capture 对象内部管理）
    std::thread video_encode_thread;
    std::thread audio_encode_thread;
    std::thread mux_thread;

    // 统计
    std::atomic<int64_t> video_frames_captured{0};
    std::atomic<int64_t> audio_frames_captured{0};
    std::atomic<int64_t> video_frames_encoded{0};
    std::atomic<int64_t> audio_frames_encoded{0};
    std::atomic<int64_t> packets_sent{0};
    std::atomic<int64_t> frames_dropped{0};

    Impl(std::unique_ptr<IVideoCapture> vc,
         std::unique_ptr<IAudioCapture> ac,
         std::unique_ptr<IVideoEncoder> ve,
         std::unique_ptr<IAudioEncoder> ae,
         std::unique_ptr<IMediaPublisher> pub)
        : video_capture(std::move(vc)),
          audio_capture(std::move(ac)),
          video_encoder(std::move(ve)),
          audio_encoder(std::move(ae)),
          publisher(std::move(pub)) {}

    // 状态迁移
    void set_state(SessionState new_state) {
        auto old = state.exchange(new_state);
        if (observer) {
            observer->on_state_changed(old, new_state);
        }
        LOG_I("session", "state %s -> %s",
              session_state_name(old), session_state_name(new_state));
    }

    // 线程：视频编码
    void video_encode_loop() {
        LOG_I("encode", "video encode thread started");

        while (!stop_source.stop_requested()) {
            VideoFrame frame;
            auto res = raw_video_queue.pop(frame, TimeDeltaUs::from_ms(5000));
            if (res == QueueResult::Aborted) break;
            if (res == QueueResult::Timeout) continue;

            auto result = video_encoder->encode(std::move(frame));
            if (result.is_err()) {
                LOG_E("encode", "video encode error: %s", result.to_string().c_str());
                continue;
            }

            for (auto& pkt : *result) {
                while (!stop_source.stop_requested()) {
                    auto push_res = video_pkt_queue.push(std::move(pkt), TimeDeltaUs::from_ms(1000));
                    if (push_res == QueueResult::Aborted) goto exit_video_loop;
                    if (push_res == QueueResult::Ok) break;
                }
                video_frames_encoded++;
            }
        }
    exit_video_loop:
        LOG_I("encode", "video encode thread exiting");
    }

    // 线程：音频编码
    void audio_encode_loop() {
        LOG_I("encode", "audio encode thread started");

        while (!stop_source.stop_requested()) {
            AudioFrame frame;
            auto res = raw_audio_queue.pop(frame, TimeDeltaUs::from_ms(5000));
            if (res == QueueResult::Aborted) break;
            if (res == QueueResult::Timeout) continue;

            auto result = audio_encoder->encode(std::move(frame));
            if (result.is_err()) {
                LOG_E("encode", "audio encode error: %s", result.to_string().c_str());
                continue;
            }

            for (auto& pkt : *result) {
                while (!stop_source.stop_requested()) {
                    auto push_res = audio_pkt_queue.push(std::move(pkt), TimeDeltaUs::from_ms(1000));
                    if (push_res == QueueResult::Aborted) goto exit_audio_loop;
                    if (push_res == QueueResult::Ok) break;
                }
                audio_frames_encoded++;
            }
        }
    exit_audio_loop:
        LOG_I("encode", "audio encode thread exiting");
    }

    // 线程：封装 + 发布（音视频交织）
    void mux_loop() {
        LOG_I("mux", "mux/publish thread started");

        while (!stop_source.stop_requested()) {
            // Peek video and audio queues, pick the one with smaller PTS
            MediaPacket vpkt;
            MediaPacket apkt;
            bool got_video = (video_pkt_queue.try_peek(vpkt) == QueueResult::Ok);
            bool got_audio = this->has_audio && (audio_pkt_queue.try_peek(apkt) == QueueResult::Ok);

            MediaPacket pkt;
            if (got_video && got_audio) {
                // Both available: interleave by PTS
                if (vpkt.pts.us <= apkt.pts.us) {
                    video_pkt_queue.try_pop(pkt);
                } else {
                    audio_pkt_queue.try_pop(pkt);
                }
            } else if (got_video) {
                video_pkt_queue.try_pop(pkt);
            } else if (got_audio) {
                audio_pkt_queue.try_pop(pkt);
            } else {
                // Neither available: block on video queue (or audio if video-only)
                auto res = video_pkt_queue.pop(pkt, TimeDeltaUs::from_ms(100));
                if (res == QueueResult::Aborted) break;
                if (res == QueueResult::Timeout) continue;
            }

            auto result = publisher->write_packet(pkt);
            if (result.is_err()) {
                LOG_E("mux", "write_packet error: %s", result.to_string().c_str());
                set_state(SessionState::Error);
                if (observer) {
                    observer->on_error(result.error_domain(), result.error_code(),
                                       result.error_message());
                }
                break;
            }
            packets_sent++;
        }
        LOG_I("mux", "mux/publish thread exiting");
    }
};

// ============================================================
// PublishSession 公开接口
// ============================================================
PublishSession::PublishSession(
    std::unique_ptr<IVideoCapture> vc,
    std::unique_ptr<IAudioCapture> ac,
    std::unique_ptr<IVideoEncoder> ve,
    std::unique_ptr<IAudioEncoder> ae,
    std::unique_ptr<IMediaPublisher> pub)
    : impl_(std::make_unique<Impl>(std::move(vc), std::move(ac),
                                    std::move(ve), std::move(ae),
                                    std::move(pub))) {}

PublishSession::~PublishSession() {
    stop();
}

Result<void> PublishSession::prepare(const PublishSessionConfig& config) {
    if (impl_->state != SessionState::Idle &&
        impl_->state != SessionState::Stopped) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                 "Session must be Idle or Stopped to prepare");
    }

    impl_->set_state(SessionState::Preparing);
    impl_->config = config;
    impl_->has_audio = config.enable_audio && impl_->audio_capture != nullptr
                       && impl_->audio_encoder != nullptr;

    // 打开视频采集
    auto ret = impl_->video_capture->open(config.video_capture);
    if (ret.is_err()) {
        impl_->set_state(SessionState::Error);
        return ret;
    }

    // 打开视频编码器
    ret = impl_->video_encoder->open(config.video_encode);
    if (ret.is_err()) {
        impl_->set_state(SessionState::Error);
        return ret;
    }

    // 打开音频采集（如果启用）
    if (impl_->has_audio) {
        ret = impl_->audio_capture->open(config.audio_capture);
        if (ret.is_err()) {
            impl_->set_state(SessionState::Error);
            return ret;
        }
        ret = impl_->audio_encoder->open(config.audio_encode);
        if (ret.is_err()) {
            impl_->set_state(SessionState::Error);
            return ret;
        }
    }

    // 打开发布器
    ret = impl_->publisher->open(config.publish);
    if (ret.is_err()) {
        impl_->set_state(SessionState::Error);
        return ret;
    }

    // 构建 StreamInfo
    StreamInfo video_info;
    video_info.type = MediaType::Video;
    video_info.codec = CodecId::H264;
    video_info.width = config.video_encode.width;
    video_info.height = config.video_encode.height;
    video_info.frame_rate = config.video_encode.frame_rate;
    video_info.pixel_format = config.video_encode.input_format;
    video_info.codec_extradata = impl_->video_encoder->extradata();
    video_info.bitrate_bps = config.video_encode.bitrate_bps;

    StreamInfo audio_info;
    if (impl_->has_audio) {
        audio_info.type = MediaType::Audio;
        audio_info.codec = CodecId::AAC;
        audio_info.sample_rate = config.audio_encode.sample_rate;
        audio_info.channels = config.audio_encode.channels;
        audio_info.sample_format = config.audio_encode.input_format;
        audio_info.codec_extradata = impl_->audio_encoder->extradata();
        audio_info.bitrate_bps = config.audio_encode.bitrate_bps;
    }

    // 写 FLV header + sequence headers
    ret = impl_->publisher->write_header(video_info, audio_info);
    if (ret.is_err()) {
        impl_->set_state(SessionState::Error);
        return ret;
    }

    impl_->set_state(SessionState::Prepared);
    return Result<void>::ok();
}

Result<void> PublishSession::start() {
    if (impl_->state != SessionState::Prepared) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                 "Session must be Prepared to start");
    }

    impl_->stop_source.reset();

    // 启动视频采集
    auto ret = impl_->video_capture->start(
        [this](VideoFrame frame) {
            impl_->video_frames_captured++;
            auto res = impl_->raw_video_queue.push(std::move(frame));
            if (res != QueueResult::Ok) {
                impl_->frames_dropped++;
            }
        },
        [this](ErrorDomain domain, ErrorCode code, std::string msg) {
            LOG_E("capture", "[%s:%d] %s",
                  error_domain_name(domain), static_cast<int>(code), msg.c_str());
            if (impl_->observer) {
                impl_->observer->on_error(domain, code, msg);
            }
        });
    if (ret.is_err()) return ret;

    // 启动音频采集（如果启用）
    if (impl_->has_audio) {
        ret = impl_->audio_capture->start(
            [this](AudioFrame frame) {
                impl_->audio_frames_captured++;
                impl_->raw_audio_queue.push(std::move(frame));
            },
            [this](ErrorDomain domain, ErrorCode code, std::string msg) {
                LOG_E("capture", "[audio] [%s:%d] %s",
                      error_domain_name(domain), static_cast<int>(code), msg.c_str());
                if (impl_->observer) {
                    impl_->observer->on_error(domain, code, msg);
                }
            });
        if (ret.is_err()) return ret;
    }

    // 启动编码线程
    impl_->video_encode_thread = std::thread(&Impl::video_encode_loop, impl_.get());
    if (impl_->has_audio) {
        impl_->audio_encode_thread = std::thread(&Impl::audio_encode_loop, impl_.get());
    }

    // 启动 mux 线程
    impl_->mux_thread = std::thread(&Impl::mux_loop, impl_.get());

    impl_->set_state(SessionState::Running);
    return Result<void>::ok();
}

void PublishSession::stop() {
    auto current = impl_->state.load();
    if (current == SessionState::Idle ||
        current == SessionState::Stopped ||
        current == SessionState::Stopping) {
        return;  // 幂等
    }

    LOG_I("session", "stopping...");
    impl_->set_state(SessionState::Stopping);

    // 1. 请求停止
    impl_->stop_source.request_stop();

    // 2. 停止采集
    impl_->video_capture->stop();
    if (impl_->has_audio) {
        impl_->audio_capture->stop();
    }

    // 3. abort 队列 → 唤醒所有阻塞的 pop
    impl_->raw_video_queue.abort();
    impl_->raw_audio_queue.abort();
    impl_->video_pkt_queue.abort();
    impl_->audio_pkt_queue.abort();

    // 4. interrupt 网络 I/O
    impl_->publisher->interrupt();

    // 5. join 线程
    if (impl_->video_encode_thread.joinable()) {
        impl_->video_encode_thread.join();
    }
    if (impl_->audio_encode_thread.joinable()) {
        impl_->audio_encode_thread.join();
    }
    if (impl_->mux_thread.joinable()) {
        impl_->mux_thread.join();
    }

    // 6. drain 编码器残余
    impl_->video_encoder->drain();
    if (impl_->has_audio) {
        impl_->audio_encoder->drain();
    }

    // 7. 关闭模块
    impl_->publisher->close();
    impl_->video_encoder->close();
    if (impl_->has_audio) {
        impl_->audio_encoder->close();
        impl_->audio_capture->close();
    }
    impl_->video_capture->close();

    impl_->set_state(SessionState::Stopped);

    // 汇总
    auto ps = impl_->publisher->stats();
    LOG_I("session", "stopped — video cap=%ld enc=%ld audio cap=%ld enc=%ld "
          "sent=%ld bytes=%ld drop=%ld",
          impl_->video_frames_captured.load(), impl_->video_frames_encoded.load(),
          impl_->audio_frames_captured.load(), impl_->audio_frames_encoded.load(),
          ps.packets_written, ps.bytes_written,
          impl_->frames_dropped.load());
}

void PublishSession::reset() {
    stop();
    impl_->set_state(SessionState::Idle);
}

SessionState PublishSession::state() const {
    return impl_->state.load();
}

void PublishSession::set_observer(std::shared_ptr<ISessionObserver> observer) {
    impl_->observer = std::move(observer);
}

ISessionObserver::Metrics PublishSession::metrics() const {
    ISessionObserver::Metrics m;
    m.frames_captured = impl_->video_frames_captured.load()
                         + impl_->audio_frames_captured.load();
    m.frames_encoded = impl_->video_frames_encoded.load()
                        + impl_->audio_frames_encoded.load();
    m.packets_sent = impl_->packets_sent.load();
    m.frames_dropped = impl_->frames_dropped.load();
    m.raw_video_queue = impl_->raw_video_queue.size();
    m.raw_audio_queue = impl_->raw_audio_queue.size();
    m.video_pkt_queue = impl_->video_pkt_queue.size();
    m.audio_pkt_queue = impl_->audio_pkt_queue.size();
    auto ps = impl_->publisher->stats();
    m.bytes_sent = ps.bytes_written;
    return m;
}

}  // namespace streambridge
