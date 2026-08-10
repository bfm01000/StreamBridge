#include "native_playback_session.h"

#include <algorithm>

#include "streambridge/logging.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeSession";

constexpr size_t kMaxPacketQueueSize = 60;
constexpr int64_t kPacketPopTimeoutMs = 200;
constexpr int64_t kDemuxReadTimeoutMs = 5000;

void log_info(const char* msg) {
    SB_LOG_I(kLogTag, "%s", msg);
}

}  // namespace

// ============================================================
// Ctor / Dtor
// ============================================================

NativePlaybackSession::NativePlaybackSession()
    : video_packet_queue_({kMaxPacketQueueSize})
    , audio_packet_queue_({kMaxPacketQueueSize}) {}

NativePlaybackSession::~NativePlaybackSession() {
    stop();
    clear_surface();
}

// ============================================================
// Public API
// ============================================================

int NativePlaybackSession::start(std::string url, ANativeWindow* window) {
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = std::move(url);
        last_error_.clear();
        abort_requested_ = false;
        surface_paused_ = false;
        stop_in_progress_ = false;
        video_frames_rendered_ = 0;
        video_frames_dropped_ = 0;
        audio_frames_output_ = 0;
        last_av_diff_us_ = 0;
        last_sync_action_ = streambridge::VideoSyncAction::Render;
        first_video_pts_us_ = -1;
        first_audio_pts_us_ = -1;
        video_info_ = nullptr;
        audio_info_ = nullptr;
    }

    video_packet_queue_.reset();
    audio_packet_queue_.reset();
    renderer_.set_surface(window);
    clock_.reset();

    set_state(streambridge::SessionState::Preparing);

#ifdef STREAMBRIDGE_BUILD_VERSION
    SB_LOG_I(kLogTag,
            "build version: " STREAMBRIDGE_BUILD_VERSION);
#endif

    demux_thread_ = std::thread(&NativePlaybackSession::demux_loop, this);
    video_thread_ = std::thread(&NativePlaybackSession::video_loop, this);
    audio_thread_ = std::thread(&NativePlaybackSession::audio_loop, this);

    log_info("playback session started");
    return 0;
}

void NativePlaybackSession::stop() {
    // Idempotent guard: only one caller executes cleanup
    bool expected = false;
    if (!stop_in_progress_.compare_exchange_strong(expected, true)) {
        return;  // already stopping
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == streambridge::SessionState::Idle ||
                state_ == streambridge::SessionState::Stopped) {
            stop_in_progress_ = false;
            return;
        }
    }

    log_info("playback session stopping");
    set_state(streambridge::SessionState::Stopping);

    request_stop();

    if (demux_thread_.joinable()) demux_thread_.join();
    if (video_thread_.joinable()) video_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();

    subscriber_.close();
    if (video_decoder_) video_decoder_->close();
    if (audio_decoder_) audio_decoder_->close();
    audio_output_.close();
    clock_.reset();

    set_state(streambridge::SessionState::Stopped);
    log_info("playback session stopped");
}

void NativePlaybackSession::set_surface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_.set_surface(window);
    // Resume rendering if we were paused due to surface loss
    if (window != nullptr && state_ == streambridge::SessionState::Paused) {
        set_state_locked(streambridge::SessionState::Running);
    }
    surface_paused_ = (window == nullptr);
}

void NativePlaybackSession::clear_surface() {
    surface_paused_ = true;  // signal video thread to stop rendering before releasing window
    {
        std::lock_guard<std::mutex> lock(mutex_);
        renderer_.clear_surface();
        if (state_ == streambridge::SessionState::Running) {
            set_state_locked(streambridge::SessionState::Paused);
        }
    }
}

streambridge::SessionState NativePlaybackSession::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string NativePlaybackSession::status_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << streambridge::session_state_name(state_);
    if (!last_error_.empty()) {
        oss << ": " << last_error_;
    }
    oss << " vid=" << video_frames_rendered_
        << " drop=" << video_frames_dropped_
        << " aud=" << audio_frames_output_
        << " sync=" << streambridge::video_sync_action_name(last_sync_action_)
        << " av_diff=" << last_av_diff_us_ << "us";
    return oss.str();
}

// ============================================================
// State management
// ============================================================

void NativePlaybackSession::set_state(streambridge::SessionState s) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = s;
}

void NativePlaybackSession::set_state_locked(streambridge::SessionState s) {
    state_ = s;
}

void NativePlaybackSession::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = msg;
    state_ = streambridge::SessionState::Error;
    abort_requested_ = true;
    video_packet_queue_.abort();
    audio_packet_queue_.abort();
    SB_LOG_E(kLogTag, "Error: %s", msg.c_str());
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
// Demux thread: RTMP pull / FLV demux
// ============================================================

void NativePlaybackSession::demux_loop() {
    log_info("demux thread started");

    std::string url_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_copy = url_;
    }

    constexpr int kMaxReconnect = 5;
    constexpr int kReconnectDelayMs = 2000;
    int reconnect_count = 0;

    // Outer loop: handles reconnection on network failure
    while (!is_stopping() && reconnect_count <= kMaxReconnect) {
        if (reconnect_count > 0) {
            SB_LOG_I(kLogTag,
                    "demux: reconnect attempt %d/%d after %dms",
                    reconnect_count, kMaxReconnect, kReconnectDelayMs);

            // Flush stale packets from queues (keep alive for decode threads)
            video_packet_queue_.flush();
            audio_packet_queue_.flush();

            // Reset PTS normalization for the new stream
            first_video_pts_us_ = -1;
            first_audio_pts_us_ = -1;

            // Reset clock (decoders stay open — same codec params expected)
            clock_.reset();

            // Notify state
            {
                std::lock_guard<std::mutex> lock(mutex_);
                video_info_ = nullptr;
                audio_info_ = nullptr;
                set_state_locked(streambridge::SessionState::Reconnecting);
            }

            // Wait before retry
            for (int i = 0; i < kReconnectDelayMs / 100 && !is_stopping(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (is_stopping()) break;
        }

        // Open subscriber
        auto open_result = subscriber_.open(url_copy);
        if (open_result.is_err()) {
            SB_LOG_W(kLogTag,
                    "demux: open failed (attempt %d): %s",
                    reconnect_count, open_result.error_message().c_str());
            ++reconnect_count;
            continue;
        }

        // Publish stream info
        {
            std::lock_guard<std::mutex> lock(mutex_);
            video_info_ = subscriber_.video_stream();
            audio_info_ = subscriber_.audio_stream();
            SB_LOG_I(kLogTag,
                    "demux: has_video=%d has_audio=%d",
                    video_info_ != nullptr, audio_info_ != nullptr);
            set_state_locked(streambridge::SessionState::Running);
        }

        clock_.start(streambridge::TimePointUs{0});
        reconnect_count = 0;  // reset on successful connection
        SB_LOG_I(kLogTag,
                "demux: connected, reading packets");

        // Read loop
        int packet_count = 0;
        bool connection_lost = false;
        while (!is_stopping()) {
            auto result = subscriber_.read_packet();
            if (++packet_count == 1) {
                SB_LOG_I(kLogTag,
                        "demux: first packet read, type=%d",
                        static_cast<int>(result->type));
            }
            if (result.is_err()) {
                SB_LOG_W(kLogTag,
                        "demux: read error: %s", result.error_message().c_str());
                connection_lost = true;
                break;
            }

            auto& packet = *result;
            if (packet.type == streambridge::MediaType::Unknown) {
                log_info("demux: EOF reached");
                connection_lost = true;  // treat EOF same as disconnect
                break;
            }

            if (packet.type == streambridge::MediaType::Video) {
                if (first_video_pts_us_ < 0 && packet.has_valid_pts()) {
                    first_video_pts_us_ = packet.pts.us;
                }
                auto push_result = video_packet_queue_.push(
                    std::move(packet),
                    streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs));
                if (push_result == streambridge::QueueResult::Aborted) {
                    connection_lost = false;  // intentional stop, not reconnect
                    break;
                }
            } else if (packet.type == streambridge::MediaType::Audio) {
                if (first_audio_pts_us_ < 0 && packet.has_valid_pts()) {
                    first_audio_pts_us_ = packet.pts.us;
                }
                auto push_result = audio_packet_queue_.push(
                    std::move(packet),
                    streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs));
                if (push_result == streambridge::QueueResult::Aborted) {
                    connection_lost = false;
                    break;
                }
            }
        }

        subscriber_.close();
        if (!connection_lost) break;  // normal stop, don't reconnect
        ++reconnect_count;
    }

    // Terminal: notify decode threads
    video_packet_queue_.abort();
    audio_packet_queue_.abort();

    if (reconnect_count > kMaxReconnect) {
        set_error("demux: max reconnect attempts exceeded");
    }

    log_info("demux thread exiting");
}

// ============================================================
// Video thread: H.264 decode + render
// ============================================================

void NativePlaybackSession::video_loop() {
    log_info("video thread started");

    {
        int waits = 0;
        while (!is_stopping()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (video_info_ != nullptr) break;
                if (state_ == streambridge::SessionState::Error) return;
                // Keep waiting during reconnect
                if (state_ == streambridge::SessionState::Reconnecting || state_ == streambridge::SessionState::Paused) {
                    // reset timeout counter while waiting
                    waits = 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (++waits > 200) {
                set_error("timeout waiting for video stream info");
                return;
            }
        }
    }
    if (is_stopping()) return;

    streambridge::StreamInfo video_info_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_info_copy = *video_info_;
    }

    SB_LOG_I(kLogTag,
            "video: opening decoder codec=%d %dx%d",
            static_cast<int>(video_info_copy.codec),
            video_info_copy.width, video_info_copy.height);

    // Create decoder via factory (MediaCodec preferred, FFmpeg fallback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_decoder_ = streambridge::android::mediacodec::create_video_decoder(
            renderer_.window());
    }
    if (!video_decoder_) {
        set_error("failed to create video decoder");
        return;
    }

    auto open_result = video_decoder_->open(video_info_copy);
    if (open_result.is_err()) {
        set_error(open_result.error_message());
        return;
    }
    log_info("video decoder opened, entering decode loop");

    // Main decode loop: send each packet ONCE, receive all ready frames
    int pkt_fed = 0;
    int frame_out = 0;
    while (!is_stopping()) {
        streambridge::MediaPacket packet;
        auto pop_result = video_packet_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) break;
        if (pop_result == streambridge::QueueResult::Timeout) continue;

        // Send packet once (PTS pushed to FIFO queue inside send_packet)
        auto send_result = video_decoder_->send_packet(packet);
        if (send_result.is_err()) {
            set_error(send_result.error_message());
            return;
        }
        if (++pkt_fed <= 5 || pkt_fed % 100 == 0) {
            SB_LOG_I(kLogTag, "video: fed pkt#%d pts=%lld size=%zu",
                     pkt_fed, static_cast<long long>(packet.pts.us), packet.data.size());
        }

        // Dequeue decoded output (frame cached inside decoder)
        while (!is_stopping()) {
            auto dec_result = video_decoder_->dequeue_output(
                kPacketPopTimeoutMs * 1000);
            if (dec_result.is_err()) {
                set_error(dec_result.error_message());
                return;
            }
            if (!dec_result->has_output) break;  // no frame ready

            if (++frame_out <= 5 || frame_out % 100 == 0) {
                SB_LOG_I(kLogTag, "video: out frame#%d pts=%lld mode=%s",
                         frame_out, static_cast<long long>(dec_result->pts_us),
                         (video_decoder_->output_mode()
                          == streambridge::IVideoDecoder::OutputMode::Surface)
                         ? "Surface" : "CpuFrame");
            }

            bool is_surface_mode = (video_decoder_->output_mode()
                == streambridge::IVideoDecoder::OutputMode::Surface);

            int64_t frame_pts_us = 0;

            if (is_surface_mode) {
                // Surface mode: PTS comes from dequeue_output, no CPU frame
                frame_pts_us = dec_result->pts_us;
                // Normalize PTS
                if (first_video_pts_us_ >= 0 && frame_pts_us > 0) {
                    frame_pts_us -= first_video_pts_us_;
                }
            } else {
                // CPU mode: retrieve frame data
                auto frame_result = video_decoder_->receive_frame(dec_result->output_index);
                if (frame_result.is_err()) {
                    set_error(frame_result.error_message());
                    return;
                }
                if (!frame_result->has_frame) {
                    video_decoder_->release_output(dec_result->output_index, false);
                    continue;
                }
                frame_pts_us = frame_result->frame.pts.us;
                // Normalize PTS
                if (first_video_pts_us_ >= 0 && frame_result->frame.pts.us > 0) {
                    frame_pts_us = frame_result->frame.pts.us - first_video_pts_us_;
                }
            }

            // AV sync: audio master clock -> video sync
            auto master_clock = clock_.now();
            auto sync = sync_controller_.decide(
                streambridge::TimePointUs{frame_pts_us}, master_clock);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_av_diff_us_ = sync.av_diff_us;
                last_sync_action_ = sync.action;
            }

            if (sync.action == streambridge::VideoSyncAction::Drop) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++video_frames_dropped_;
                video_decoder_->release_output(dec_result->output_index, false);
                continue;
            }

            if (sync.action == streambridge::VideoSyncAction::Wait) {
                int64_t sleep_us = sync.wait_us;  // wait full duration (clock catches up naturally)
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                master_clock = clock_.now();
                sync = sync_controller_.decide(
                    streambridge::TimePointUs{frame_pts_us}, master_clock);
                if (sync.action == streambridge::VideoSyncAction::Drop) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++video_frames_dropped_;
                    video_decoder_->release_output(dec_result->output_index, false);
                    continue;
                }
            }

            // Wait if surface is paused (e.g., activity background)
            while (surface_paused_.load(std::memory_order_acquire) && !is_stopping()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (is_stopping()) break;

            // Render (CPU mode: ANativeWindow; Surface mode: MediaCodec release)
            if (is_surface_mode) {
                // Surface mode: release buffer to render on Surface
                video_decoder_->release_output(dec_result->output_index, true);
                std::lock_guard<std::mutex> lock(mutex_);
                ++video_frames_rendered_;
            } else {
                // CPU mode: get frame from receive_frame result
                auto frame_result2 = video_decoder_->receive_frame(dec_result->output_index);
                if (frame_result2.is_ok() && frame_result2->has_frame) {
                    auto& frame = frame_result2->frame;
                    auto render_result = renderer_.render(frame);
                    if (video_frames_rendered_ == 0) {
                        SB_LOG_I(kLogTag,
                                "video: first frame rendered %dx%d fmt=%d pts=%lld",
                                frame.width, frame.height,
                                static_cast<int>(frame.format),
                                static_cast<long long>(frame_pts_us));
                    }
                    if (render_result.is_err()) {
                        SB_LOG_W(kLogTag, "render failed: %s",
                                         render_result.error_message().c_str());
                    } else {
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++video_frames_rendered_;
                    }
                }
                video_decoder_->release_output(dec_result->output_index, false);
            }

            if (video_frames_rendered_ % 100 == 0) {
                SB_LOG_I(kLogTag,
                        "video: frame=%lld pts=%lld av=%lld q=%zu",
                        static_cast<long long>(video_frames_rendered_),
                        static_cast<long long>(frame_pts_us),
                        static_cast<long long>(sync.av_diff_us),
                        video_packet_queue_.size());
            }

            // Periodic stability log (every ~5s of media time)
            {
                static int64_t last_log_pts_us = 0;
                if (frame_pts_us - last_log_pts_us >= 5'000'000) {
                    last_log_pts_us = frame_pts_us;
                    SB_LOG_I(kLogTag,
                            "stability: pts=%lld rendered=%lld dropped=%lld "
                            "vq=%zu aq=%zu av=%lld state=%s",
                            static_cast<long long>(frame_pts_us),
                            static_cast<long long>(video_frames_rendered_),
                            static_cast<long long>(video_frames_dropped_),
                            video_packet_queue_.size(),
                            audio_packet_queue_.size(),
                            static_cast<long long>(sync.av_diff_us),
                            streambridge::session_state_name(state_));
                }
            }
        }
    }

    // Drain decoder
    while (!is_stopping()) {
        video_decoder_->drain();
    }

    video_decoder_->close();
    log_info("video thread exiting");
}

// ============================================================
// Audio thread: AAC decode + AAudio output + clock update
// ============================================================

void NativePlaybackSession::audio_loop() {
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_info_ == nullptr) {
            log_info("no audio stream, audio thread exiting");
            return;
        }
    }
    if (is_stopping()) return;

    streambridge::StreamInfo audio_info_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_info_copy = *audio_info_;
    }

    // Create audio decoder (FFmpeg AAC)
    audio_decoder_ = streambridge::android::mediacodec::create_audio_decoder();
    if (!audio_decoder_) {
        set_error("failed to create audio decoder");
        return;
    }

    auto open_result = audio_decoder_->open(audio_info_copy);
    if (open_result.is_err()) {
        set_error(open_result.error_message());
        return;
    }

    auto audio_open = audio_output_.open(audio_info_copy.sample_rate, audio_info_copy.channels);
    if (audio_open.is_err()) {
        set_error(audio_open.error_message());
        return;
    }
    log_info("audio decoder and AAudio output opened");

    int64_t local_first_audio_pts = first_audio_pts_us_;

    while (!is_stopping()) {
        streambridge::MediaPacket packet;
        auto pop_result = audio_packet_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) break;
        if (pop_result == streambridge::QueueResult::Timeout) continue;

        // Send packet once (PTS pushed to FIFO queue inside send_packet)
        auto send_result = audio_decoder_->send_packet(packet);
        if (send_result.is_err()) {
            set_error(send_result.error_message());
            return;
        }

        // Receive all ready frames
        while (!is_stopping()) {
            auto dec_result = audio_decoder_->receive_frame();
            if (dec_result.is_err()) {
                set_error(dec_result.error_message());
                return;
            }
            if (!dec_result->has_frame) break;

            auto& frame = dec_result->frame;

            if (local_first_audio_pts >= 0 && frame.pts.us > 0) {
                frame.pts.us -= local_first_audio_pts;
            }

            auto write_result = audio_output_.write(frame);
            if (write_result.is_err()) {
                SB_LOG_W(kLogTag,
                                    "audio write failed: %s",
                                    write_result.error_message().c_str());
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                ++audio_frames_output_;
            }

            clock_.update_audio(
                streambridge::TimePointUs{0},  // anchor at 0 (all PTS normalized)
                audio_output_.played_frames(),
                audio_info_copy.sample_rate);
        }
    }

    audio_decoder_->drain();

    audio_decoder_->close();
    audio_output_.close();
    log_info("audio thread exiting");
}

}  // namespace streambridge::android
