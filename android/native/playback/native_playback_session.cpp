#include "native_playback_session.h"

#include <algorithm>

#include "streambridge/logging.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>

#include "ffmpeg/ffmpeg_video_decoder.h"
#include "playback_constants.h"
#include "playback_queue_config.h"
#include "playback_reconnect_controller.h"

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeSession";

void log_info(const char* msg) {
    SB_LOG_I(kLogTag, "%s", msg);
}

}  // namespace

// ============================================================
// Ctor / Dtor
// ============================================================

NativePlaybackSession::NativePlaybackSession()
    : video_packet_queue_(compressed_packet_queue_config())
    , audio_packet_queue_(compressed_packet_queue_config()) {}

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
        first_video_pts_us_ = -1;
        first_audio_pts_us_ = -1;
        video_info_ = nullptr;
        audio_info_ = nullptr;
    }
    metrics_.reset();

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
    bool surface_changed = false;
    ANativeWindow* decoder_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        surface_changed = (renderer_.window() != window);
        if (surface_changed) {
            renderer_.set_surface(window);
        }
        // Resume rendering if we were paused due to surface loss
        if (window != nullptr && state_ == streambridge::SessionState::Paused) {
            set_state_locked(streambridge::SessionState::Running);
        }
        surface_paused_ = (window == nullptr);
        decoder_window = renderer_.window();
    }

    if (!surface_changed) {
        return;
    }

    {
        std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
        if (video_decoder_) {
            auto result = streambridge::android::mediacodec::set_decoder_surface(
                video_decoder_.get(), decoder_window);
            if (result.is_err()) {
                SB_LOG_W(kLogTag, "MediaCodec set_surface failed: %s",
                         result.error_message().c_str());
            }
        }
    }
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
    oss << metrics_.status_suffix();
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

    PlaybackReconnectController reconnect(5, 2000);

    // Outer loop: handles reconnection on network failure
    while (!is_stopping() && reconnect.can_try()) {
        if (reconnect.is_reconnecting()) {
            SB_LOG_I(kLogTag,
                    "demux: reconnect attempt %d/%d after %dms",
                    reconnect.attempt(), reconnect.max_attempts(), reconnect.delay_ms());

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
            for (int i = 0; i < reconnect.delay_ms() / 100 && !is_stopping(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (is_stopping()) break;
        }

        // Open subscriber
        auto open_result = subscriber_.open(url_copy);
        if (open_result.is_err()) {
            SB_LOG_W(kLogTag,
                    "demux: open failed (attempt %d): %s",
                    reconnect.attempt(), open_result.error_message().c_str());
            reconnect.record_failure();
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
        reconnect.reset_after_connected();
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
                if (push_result == streambridge::QueueResult::Timeout) {
                    SB_LOG_W(kLogTag, "demux: video packet queue blocked for %lldms, reconnect",
                             static_cast<long long>(kDemuxReadTimeoutMs));
                    connection_lost = true;
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
                if (push_result == streambridge::QueueResult::Timeout) {
                    SB_LOG_W(kLogTag, "demux: audio packet queue blocked for %lldms, reconnect",
                             static_cast<long long>(kDemuxReadTimeoutMs));
                    connection_lost = true;
                    break;
                }
            }
        }

        subscriber_.close();
        if (!connection_lost) break;  // normal stop, don't reconnect
        reconnect.record_failure();
    }

    // Terminal: notify decode threads
    video_packet_queue_.abort();
    audio_packet_queue_.abort();

    if (reconnect.exhausted()) {
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
        SB_LOG_W(kLogTag, "video: hardware decoder open failed, fallback to software: %s",
                 open_result.error_message().c_str());
        video_decoder_ = std::make_unique<streambridge::ffmpeg::FFmpegVideoDecoder>();
        open_result = video_decoder_->open(video_info_copy);
        if (open_result.is_err()) {
            set_error(open_result.error_message());
            return;
        }
    }
    SB_LOG_I(kLogTag, "video decoder opened, mode=%s",
             video_decoder_->capability().supports_surface_output ? "MediaCodec zero-copy" : "FFmpeg software");

    // Main decode loop: send each packet ONCE, receive all ready frames
    int pkt_fed = 0;
    int frame_out = 0;
    int pkt_drop = 0;
    auto last_heartbeat = std::chrono::steady_clock::now();
    while (!is_stopping()) {
        // Heartbeat: prove thread is alive every 2s
        auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat > std::chrono::seconds(2)) {
            last_heartbeat = now;
            SB_LOG_I(kLogTag,
                "video: HEARTBEAT pkt_fed=%d frame_out=%d dropped=%d vq=%zu aq=%zu "
                "has_audio=%d sync=%s",
                pkt_fed, frame_out, pkt_drop,
                video_packet_queue_.size(), audio_packet_queue_.size(),
                clock_.has_audio_clock() ? 1 : 0,
                streambridge::video_sync_action_name(metrics_.last_sync_action()));
        }

        streambridge::MediaPacket packet;
        auto pop_result = video_packet_queue_.pop(
            packet,
            streambridge::TimeDeltaUs::from_ms(kPacketPopTimeoutMs));
        if (pop_result == streambridge::QueueResult::Aborted) {
            SB_LOG_I(kLogTag, "video: pop ABORTED, exiting");
            break;
        }
        if (pop_result == streambridge::QueueResult::Timeout) {
            continue;  // silent retry
        }

        ++pkt_fed;
        metrics_.on_video_packet_fed();

        // Send packet once
        Result<DecodeStatus> send_result = Result<DecodeStatus>::ok(DecodeStatus::TryAgain);
        {
            std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
            send_result = video_decoder_->send_packet(packet);
        }
        if (send_result.is_err()) {
            set_error(send_result.error_message());
            return;
        }
        if (pkt_fed <= 10 || pkt_fed % 50 == 0) {
            SB_LOG_I(kLogTag, "video: fed pkt#%d pts=%lld size=%zu q=%zu",
                     pkt_fed, static_cast<long long>(packet.pts.us),
                     packet.data.size(), video_packet_queue_.size());
        }

        // Receive ALL ready frames
        int deq_loops = 0;
        while (!is_stopping()) {
            ++deq_loops;
            Result<DecodeOutput> recv = Result<DecodeOutput>::err(
                ErrorDomain::Internal, ErrorCode::QueueTimeout, "not ready");
            {
                std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
                recv = video_decoder_->receive_frame(kVideoDrainTimeoutMs);
            }
            if (recv.is_err()) {
                if (recv.error_code() == ErrorCode::QueueTimeout) {
                    if (deq_loops == 1 && pkt_fed <= 10)
                        SB_LOG_I(kLogTag, "video: TryAgain after pkt#%d", pkt_fed);
                    break;
                }
                set_error(recv.error_message());
                return;
            }

            auto& out = *recv;
            ++frame_out;
            metrics_.on_video_frame_decoded();
            int64_t norm_pts = (first_video_pts_us_ >= 0 && out.pts_us > 0)
                ? out.pts_us - first_video_pts_us_ : out.pts_us;

            // AV sync
            bool do_sync = clock_.has_audio_clock();
            auto sync = streambridge::VideoSyncDecision{};
            if (do_sync) {
                auto mc = clock_.now();
                sync = sync_controller_.decide(
                    streambridge::TimePointUs{norm_pts}, mc);
                metrics_.set_sync(sync.action, sync.av_diff_us);
            }

            if (frame_out <= 10 || frame_out % 50 == 0)
                SB_LOG_I(kLogTag, "video: frame#%d pts=%lld av=%lld act=%s",
                         frame_out, (long long)norm_pts, (long long)sync.av_diff_us,
                         video_sync_action_name(do_sync ? sync.action : VideoSyncAction::Render));

            if (do_sync && sync.action == VideoSyncAction::Drop) {
                ++pkt_drop;
                metrics_.on_video_frame_dropped();
                {
                    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
                    video_decoder_->discard_frame(out.frame_id);
                }
                continue;
            }
            if (do_sync && sync.action == VideoSyncAction::Wait) {
                std::this_thread::sleep_for(std::chrono::microseconds(sync.wait_us));
                auto mc2 = clock_.now();
                sync = sync_controller_.decide(TimePointUs{norm_pts}, mc2);
                if (sync.action == VideoSyncAction::Drop) {
                    ++pkt_drop;
                    metrics_.on_video_frame_dropped();
                    {
                        std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
                        video_decoder_->discard_frame(out.frame_id);
                    }
                    continue;
                }
            }

            while (surface_paused_.load(std::memory_order_acquire) && !is_stopping())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (is_stopping()) break;

            // Render by payload type
            std::visit([&](auto&& handle) {
                using T = std::decay_t<decltype(handle)>;
                if constexpr (std::is_same_v<T, CpuFrameHandle>) {
                    if (handle.frame.is_valid()) {
                        static int rc = 0;
                        if (++rc <= 3) SB_LOG_I(kLogTag,
                            "render cpu %dx%d data=%p buf=%p stride=%d",
                            handle.frame.width, handle.frame.height,
                            (void*)handle.frame.planes[0].data,
                            (void*)(handle.buffer ? handle.buffer->data() : nullptr),
                            handle.frame.planes[0].stride);
                        renderer_.render(handle.frame);
                    }
                    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
                    video_decoder_->discard_frame(out.frame_id);
                } else if constexpr (std::is_same_v<T, DecoderSurfaceHandle>) {
                    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
                    video_decoder_->present_frame(out.frame_id, 0);
                }
                // DmaBufFrameHandle / GpuTextureHandle: future
            }, out.payload);

            metrics_.on_video_frame_presented();

            const int64_t presented = metrics_.video_frames_presented();
            if (presented > 0 && presented % 100 == 0) {
                SB_LOG_I(kLogTag,
                        "video: frame=%lld pts=%lld av=%lld q=%zu",
                        static_cast<long long>(presented),
                        static_cast<long long>(norm_pts),
                        static_cast<long long>(sync.av_diff_us),
                        video_packet_queue_.size());
            }

            // Periodic stability log (every ~5s of media time)
            {
                static int64_t last_log_pts_us = 0;
                if (norm_pts - last_log_pts_us >= 5'000'000) {
                    last_log_pts_us = norm_pts;
                    SB_LOG_I(kLogTag,
                            "stability: pts=%lld rendered=%lld dropped=%lld "
                            "vq=%zu aq=%zu av=%lld state=%s",
                            static_cast<long long>(norm_pts),
                            static_cast<long long>(metrics_.video_frames_presented()),
                            static_cast<long long>(metrics_.video_frames_dropped()),
                            video_packet_queue_.size(),
                            audio_packet_queue_.size(),
                            static_cast<long long>(sync.av_diff_us),
                            streambridge::session_state_name(state_));
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
        if (video_decoder_) {
            video_decoder_->drain();
            video_decoder_->close();
        }
    }
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

            // Start AAudio on first frame (avoids playing silence skewing clock)
            audio_output_.start();

            auto write_result = audio_output_.write(frame);
            if (write_result.is_err()) {
                SB_LOG_W(kLogTag,
                                    "audio write failed: %s",
                                    write_result.error_message().c_str());
            } else {
                metrics_.on_audio_frame_output();
            }

            // Audio master clock: anchor at frame PTS, advance via played_frames
            // RequestStart only called on first frame → no silence offset
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
