#include "native_playback_session.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeSession";

// 队列容量配置
constexpr size_t kMaxPacketQueueSize = 60;
constexpr int64_t kPacketPopTimeoutMs = 200;     // packet 队列 pop 超时
constexpr int64_t kDemuxReadTimeoutMs = 5000;    // demux read 超时

void log_info(const char* msg) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", msg);
}

const char* state_name(streambridge::SessionState s) {
    switch (s) {
        case streambridge::SessionState::Idle:         return "Idle";
        case streambridge::SessionState::Preparing:    return "Preparing";
        case streambridge::SessionState::Prepared:     return "Prepared";
        case streambridge::SessionState::Running:      return "Running";
        case streambridge::SessionState::Paused:       return "Paused";
        case streambridge::SessionState::Reconnecting: return "Reconnecting";
        case streambridge::SessionState::Stopping:     return "Stopping";
        case streambridge::SessionState::Stopped:      return "Stopped";
        case streambridge::SessionState::Error:        return "Error";
    }
    return "Unknown";
}

}  // namespace

// ============================================================
// 构造 / 析构
// ============================================================

NativePlaybackSession::NativePlaybackSession()
    : video_packet_queue_({kMaxPacketQueueSize})
    , audio_packet_queue_({kMaxPacketQueueSize}) {}

NativePlaybackSession::~NativePlaybackSession() {
    stop();
    clear_surface();
}

// ============================================================
// 公共 API
// ============================================================

int NativePlaybackSession::start(std::string url, ANativeWindow* window) {
    // 先停止已有会话
    stop();

    if (url.empty()) {
        last_error_ = "empty url";
        set_state(streambridge::SessionState::Error);
        return -2;
    }
    if (window == nullptr) {
        last_error_ = "surface is not ready";
        set_state(streambridge::SessionState::Error);
        return -3;
    }

    // 重置状态
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = std::move(url);
        last_error_.clear();
        abort_requested_ = false;
        video_frames_rendered_ = 0;
        video_frames_dropped_ = 0;
        audio_frames_output_ = 0;
        last_av_diff_us_ = 0;
        last_sync_action_ = VideoSyncAction::Render;
        first_video_pts_us_ = -1;
        first_audio_pts_us_ = -1;
        video_info_ = nullptr;
        audio_info_ = nullptr;
    }

    // 重置队列
    video_packet_queue_.reset();
    audio_packet_queue_.reset();

    // 设置 Surface
    renderer_.set_surface(window);

    // 重置时钟
    clock_.reset();

    set_state(streambridge::SessionState::Preparing);

    // 启动工作线程
    demux_thread_ = std::thread(&NativePlaybackSession::demux_loop, this);
    video_thread_ = std::thread(&NativePlaybackSession::video_loop, this);
    audio_thread_ = std::thread(&NativePlaybackSession::audio_loop, this);

    log_info("playback session started");
    return 0;
}

void NativePlaybackSession::stop() {
    if (state_ == streambridge::SessionState::Idle ||
            state_ == streambridge::SessionState::Stopped) {
        return;
    }

    log_info("playback session stopping");
    set_state(streambridge::SessionState::Stopping);

    request_stop();

    // 等待线程结束
    if (demux_thread_.joinable()) demux_thread_.join();
    if (video_thread_.joinable()) video_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();

    // 清理组件
    subscriber_.close();
    video_decoder_.close();
    audio_decoder_.close();
    audio_output_.close();
    clock_.reset();

    set_state(streambridge::SessionState::Stopped);
    log_info("playback session stopped");
}

void NativePlaybackSession::set_surface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_.set_surface(window);
}

void NativePlaybackSession::clear_surface() {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_.clear_surface();
    if (state_ == streambridge::SessionState::Running) {
        set_state(streambridge::SessionState::Paused);
    }
}

streambridge::SessionState NativePlaybackSession::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string NativePlaybackSession::status_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << state_name(state_);
    if (!last_error_.empty()) {
        oss << ": " << last_error_;
    }
    oss << " vid=" << video_frames_rendered_
        << " drop=" << video_frames_dropped_
        << " aud=" << audio_frames_output_
        << " sync=" << video_sync_action_name(last_sync_action_)
        << " av_diff=" << last_av_diff_us_ << "us";
    return oss.str();
}

// ============================================================
// 状态管理
// ============================================================

void NativePlaybackSession::set_state(streambridge::SessionState s) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = s;
}

void NativePlaybackSession::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = msg;
    state_ = streambridge::SessionState::Error;
    // 通知所有线程退出
    abort_requested_ = true;
    video_packet_queue_.abort();
    audio_packet_queue_.abort();
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Error: %s", msg.c_str());
}

void NativePlaybackSession::request_stop() {
    abort_requested_ = true;
    video_packet_queue_.abort();
    audio_packet_queue_.abort();
}

bool NativePlaybackSession::is_stopping() const {
    return abort_requested_.load(std::memory_order_acquire);
}

// ============================================================
// Demux 线程 — RTMP 拉流 / FLV 解封装
// ============================================================

void NativePlaybackSession::demux_loop() {
    log_info("demux thread started");

    // 打开 RTMP 连接
    std::string url_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_copy = url_;
    }

    auto open_result = subscriber_.open(url_copy);
    if (open_result.is_err()) {
        set_error(open_result.error_message());
        return;
    }

    // 获取流信息
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_info_ = subscriber_.video_stream();
        audio_info_ = subscriber_.audio_stream();
        set_state(streambridge::SessionState::Running);
    }
    clock_.start_wall_clock(streambridge::TimePointUs{0});

    log_info("demux: RTMP connected, reading packets");

    // 主读取循环
    while (!is_stopping()) {
        auto result = subscriber_.read_packet();
        if (result.is_err()) {
            set_error(result.error_message());
            break;
        }

        auto& packet = *result;
        if (packet.type == streambridge::MediaType::Unknown) {
            // EOF
            log_info("demux: EOF reached");
            break;
        }

        if (packet.type == streambridge::MediaType::Video) {
            // 记录首帧 PTS
            if (first_video_pts_us_ < 0 && packet.has_valid_pts()) {
                first_video_pts_us_ = packet.pts.us;
            }
            auto push_result = video_packet_queue_.push(
                std::move(packet),
                streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs));
            if (push_result == streambridge::QueueResult::Aborted) break;
        } else if (packet.type == streambridge::MediaType::Audio) {
            if (first_audio_pts_us_ < 0 && packet.has_valid_pts()) {
                first_audio_pts_us_ = packet.pts.us;
            }
            auto push_result = audio_packet_queue_.push(
                std::move(packet),
                streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs));
            if (push_result == streambridge::QueueResult::Aborted) break;
        }
    }

    // 通知解码线程：没有更多数据
    video_packet_queue_.abort();
    audio_packet_queue_.abort();
    log_info("demux thread exiting");
}

// ============================================================
// Video 线程 — H.264 解码 + AV 同步 + ANativeWindow 渲染
// ============================================================

void NativePlaybackSession::video_loop() {
    log_info("video thread started");

    // 等待 demux 线程提供流信息
    {
        int waits = 0;
        while (!is_stopping()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (video_info_ != nullptr) break;
                if (state_ == streambridge::SessionState::Error) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (++waits > 200) {  // 10 秒超时
                set_error("timeout waiting for video stream info");
                return;
            }
        }
    }
    if (is_stopping()) return;

    // 打开解码器
    streambridge::StreamInfo video_info_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_info_copy = *video_info_;
    }

    auto open_result = video_decoder_.open(video_info_copy);
    if (open_result.is_err()) {
        set_error(open_result.error_message());
        return;
    }
    log_info("video decoder opened");

    // 解码 + 渲染循环
    while (!is_stopping()) {
        streambridge::MediaPacket packet;
        auto pop_result = video_packet_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) break;
        if (pop_result == streambridge::QueueResult::Timeout) continue;

        // 解码（循环接收所有产出帧）
        while (!is_stopping()) {
            auto dec_result = video_decoder_.decode(packet);
            if (dec_result.is_err()) {
                set_error(dec_result.error_message());
                return;
            }
            if (!dec_result->has_frame) break;  // EAGAIN: 需要更多输入

            auto& frame = dec_result->frame;

            // 首帧 PTS 归一化
            if (first_video_pts_us_ >= 0 && frame.pts.us > 0) {
                frame.pts.us -= first_video_pts_us_;
            }

            // AV 同步决策
            const auto master_clock = clock_.current_media_time_us();
            const auto sync = sync_controller_.decide(frame.pts, master_clock);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_av_diff_us_ = sync.av_diff_us;
                last_sync_action_ = sync.action;
            }

            if (sync.action == VideoSyncAction::Drop) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++video_frames_dropped_;
                continue;
            }

            if (sync.action == VideoSyncAction::Wait) {
                if (sync.wait_us > 0 && sync.wait_us < 50'000) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(sync.wait_us));
                }
            }

            // 渲染
            auto render_result = renderer_.render(frame);
            if (render_result.is_err()) {
                __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                    "render failed: %s",
                                    render_result.error_message().c_str());
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                ++video_frames_rendered_;
            }
        }
    }

    // 冲刷解码器
    while (!is_stopping()) {
        auto drain_result = video_decoder_.drain();
        if (drain_result.is_err() || !drain_result->has_frame) break;
    }

    video_decoder_.close();
    log_info("video thread exiting");
}

// ============================================================
// Audio 线程 — AAC 解码 + AAudio 输出 + 时钟更新
// ============================================================

void NativePlaybackSession::audio_loop() {
    // 等待 demux 提供流信息
    {
        int waits = 0;
        while (!is_stopping()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (audio_info_ != nullptr || state_ == streambridge::SessionState::Error) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (++waits > 200) break;
        }
    }

    // 没有音频流 — 线程直接退出，时钟使用 wall clock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_info_ == nullptr) {
            log_info("no audio stream, audio thread exiting (clock stays on wall clock)");
            return;
        }
    }
    if (is_stopping()) return;

    // 打开解码器
    streambridge::StreamInfo audio_info_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_info_copy = *audio_info_;
    }

    auto open_result = audio_decoder_.open(audio_info_copy);
    if (open_result.is_err()) {
        set_error(open_result.error_message());
        return;
    }

    // 打开 AAudio 输出
    auto audio_open = audio_output_.open(audio_info_copy.sample_rate, audio_info_copy.channels);
    if (audio_open.is_err()) {
        set_error(audio_open.error_message());
        return;
    }
    log_info("audio decoder and AAudio output opened");

    // 首帧 PTS
    int64_t local_first_audio_pts = first_audio_pts_us_;

    // 解码 + 输出循环
    while (!is_stopping()) {
        streambridge::MediaPacket packet;
        auto pop_result = audio_packet_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) break;
        if (pop_result == streambridge::QueueResult::Timeout) continue;

        // 解码
        while (!is_stopping()) {
            auto dec_result = audio_decoder_.decode(packet);
            if (dec_result.is_err()) {
                set_error(dec_result.error_message());
                return;
            }
            if (!dec_result->has_frame) break;  // EAGAIN: 需要更多输入

            auto& frame = dec_result->frame;

            // PTS 归一化
            if (local_first_audio_pts >= 0 && frame.pts.us > 0) {
                frame.pts.us -= local_first_audio_pts;
            }

            // 输出到 AAudio
            auto write_result = audio_output_.write(frame);
            if (write_result.is_err()) {
                __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                    "audio write failed: %s",
                                    write_result.error_message().c_str());
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                ++audio_frames_output_;
            }

            // 更新音频时钟
            clock_.update_audio_position(
                streambridge::TimePointUs{local_first_audio_pts >= 0 ? local_first_audio_pts : 0},
                audio_output_.played_frames(),
                audio_info_copy.sample_rate);
        }
    }

    // 冲刷解码器
    while (!is_stopping()) {
        auto drain_result = audio_decoder_.drain();
        if (drain_result.is_err() || !drain_result->has_frame) break;
    }

    audio_decoder_.close();
    audio_output_.close();
    log_info("audio thread exiting");
}

}  // namespace streambridge::android
