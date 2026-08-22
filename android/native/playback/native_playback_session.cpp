#include "native_playback_session.h"

#include "streambridge/logging.h"
#include <sstream>
#include <utility>

#include "playback_queue_config.h"

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

int NativePlaybackSession::start(
        std::string url,
        ANativeWindow* window,
        VideoDecodePath decode_path) {
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
        input_mode_ = InputMode::Rtmp;
        rtp_local_port_ = 0;
        rtp_video_width_ = 0;
        rtp_video_height_ = 0;
        rtp_video_frame_rate_ = 0.0;
        decode_path_ = decode_path;
        last_error_.clear();
        abort_requested_ = false;
        surface_paused_ = false;
        stop_in_progress_ = false;
        first_video_pts_us_ = -1;
        first_audio_pts_us_ = -1;
        video_info_.reset();
        audio_info_.reset();
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
int NativePlaybackSession::start_rtp_video(
        uint16_t local_port,
        ANativeWindow* window,
        int width,
        int height,
        double frame_rate,
        VideoDecodePath decode_path) {
    stop();

    if (local_port == 0) {
        last_error_ = "RTP local port is required";
        set_state(streambridge::SessionState::Error);
        return -2;
    }
    if (window == nullptr) {
        last_error_ = "surface is not ready";
        set_state(streambridge::SessionState::Error);
        return -3;
    }
    if (width <= 0 || height <= 0) {
        last_error_ = "RTP video geometry is invalid";
        set_state(streambridge::SessionState::Error);
        return -4;
    }

    streambridge::StreamInfo video_info;
    video_info.type = streambridge::MediaType::Video;
    video_info.codec = streambridge::CodecId::H264;
    video_info.width = width;
    video_info.height = height;
    video_info.frame_rate = frame_rate > 0.0 ? frame_rate : 30.0;
    video_info.time_base = streambridge::Rational::micros();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_.clear();
        input_mode_ = InputMode::RtpUdpVideo;
        rtp_local_port_ = local_port;
        rtp_video_width_ = width;
        rtp_video_height_ = height;
        rtp_video_frame_rate_ = video_info.frame_rate;
        decode_path_ = decode_path;
        last_error_.clear();
        abort_requested_ = false;
        surface_paused_ = false;
        stop_in_progress_ = false;
        first_video_pts_us_ = -1;
        first_audio_pts_us_ = -1;
        video_info_ = std::move(video_info);
        audio_info_.reset();
    }
    metrics_.reset();

    video_packet_queue_.reset();
    audio_packet_queue_.reset();
    renderer_.set_surface(window);
    clock_.reset();

    set_state(streambridge::SessionState::Preparing);

    demux_thread_ = std::thread(&NativePlaybackSession::rtp_video_receive_loop, this);
    video_thread_ = std::thread(&NativePlaybackSession::video_loop, this);

    SB_LOG_I(kLogTag, "RTP video playback started local_port=%u %dx%d@%.2f",
             local_port, width, height, rtp_video_frame_rate_);
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

    if (rtp_video_receiver_) rtp_video_receiver_->close();
    if (video_worker_) video_worker_->close();
    if (audio_worker_) audio_worker_->close();
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (video_worker_) {
            auto result = video_worker_->set_surface(decoder_window);
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
    oss << metrics_.status_suffix(clock_.now().us);
    oss << " path=" << video_decode_path_name(decode_path_);
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
    if (rtp_video_receiver_) {
        rtp_video_receiver_->interrupt();
    }
    SB_LOG_E(kLogTag, "Error: %s", msg.c_str());
}

void NativePlaybackSession::request_stop() {
    abort_requested_ = true;
    video_packet_queue_.abort();
    audio_packet_queue_.abort();
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtp_video_receiver_) {
        rtp_video_receiver_->interrupt();
    }
}

bool NativePlaybackSession::is_stopping() const {
    return abort_requested_.load(std::memory_order_acquire);
}

// ============================================================
// Demux thread: RTMP pull / FLV demux
// ============================================================

void NativePlaybackSession::demux_loop() {
    std::string url_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_copy = url_;
    }

    DemuxWorker::Callbacks callbacks;
    callbacks.is_stopping = [this]() { return is_stopping(); };
    callbacks.on_stream_info = [this](const streambridge::StreamInfo* video,
                                      const streambridge::StreamInfo* audio) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (video != nullptr) {
            video_info_ = *video;
        } else {
            video_info_.reset();
        }
        if (audio != nullptr) {
            audio_info_ = *audio;
        } else {
            audio_info_.reset();
        }
        SB_LOG_I(kLogTag,
                "demux: has_video=%d has_audio=%d",
                video_info_.has_value(), audio_info_.has_value());
    };
    callbacks.on_reconnecting = [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        video_info_.reset();
        audio_info_.reset();
        set_state_locked(streambridge::SessionState::Reconnecting);
    };
    callbacks.on_running = [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        set_state_locked(streambridge::SessionState::Running);
    };
    callbacks.on_error = [this](const std::string& message) {
        set_error(message);
    };

    auto worker = std::make_unique<DemuxWorker>(
        video_packet_queue_,
        audio_packet_queue_,
        first_video_pts_us_,
        first_audio_pts_us_,
        clock_,
        metrics_,
        std::move(callbacks));
    DemuxWorker* worker_ptr = worker.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        demux_worker_ = std::move(worker);
    }
    worker_ptr->run(url_copy);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (demux_worker_.get() == worker_ptr) {
            demux_worker_.reset();
        }
    }
}

// ============================================================
// RTP video thread: UDP/RTP/H.264 -> compressed video queue
// ============================================================

void NativePlaybackSession::rtp_video_receive_loop() {
    uint16_t local_port = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local_port = rtp_local_port_;
    }

    auto receiver = std::make_unique<AndroidRtpUdpVideoReceiver>();
    AndroidRtpUdpVideoReceiverConfig config;
    config.local_port = local_port;
    auto open_result = receiver->open(config);
    if (open_result.is_err()) {
        set_error("RTP video receiver open failed: " + open_result.error_message());
        return;
    }

    AndroidRtpUdpVideoReceiver* receiver_ptr = receiver.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rtp_video_receiver_ = std::move(receiver);
        set_state_locked(streambridge::SessionState::Running);
    }
    clock_.start(streambridge::TimePointUs{0});
    SB_LOG_I(kLogTag, "RTP video receiver listening on UDP port %u", receiver_ptr->local_port());

    while (!is_stopping()) {
        auto packet = receiver_ptr->read_frame();
        if (packet.is_err()) {
            if (!is_stopping()) {
                set_error("RTP video receiver read failed: " + packet.error_message());
            }
            break;
        }
        if (first_video_pts_us_ < 0 && packet->has_valid_pts()) {
            first_video_pts_us_ = packet->pts.us;
        }
        metrics_.on_packet_demuxed(*packet);
        auto push_result = video_packet_queue_.push(
            std::move(*packet),
            streambridge::TimeDeltaUs::from_ms(2000));
        if (push_result == streambridge::QueueResult::Aborted) {
            break;
        }
        if (push_result == streambridge::QueueResult::Timeout) {
            set_error("RTP video packet queue blocked");
            break;
        }
    }

    const auto stats = receiver_ptr->stats();
    SB_LOG_I(kLogTag,
             "RTP video receiver exiting udp=%llu malformed=%llu lost=%llu reordered=%llu dropped=%llu",
             static_cast<unsigned long long>(stats.udp_datagrams),
             static_cast<unsigned long long>(stats.malformed_datagrams),
             static_cast<unsigned long long>(stats.depacketizer.lost_packets),
             static_cast<unsigned long long>(stats.depacketizer.reordered_packets),
             static_cast<unsigned long long>(stats.depacketizer.dropped_frames));

    video_packet_queue_.abort();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rtp_video_receiver_.get() == receiver_ptr) {
            rtp_video_receiver_.reset();
        }
    }
}
// ============================================================
// Video thread: H.264 decode + render
// ============================================================

void NativePlaybackSession::video_loop() {
    VideoDecodePath decode_path = VideoDecodePath::Auto;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decode_path = decode_path_;
    }

    VideoDecodeWorker::Callbacks callbacks;
    callbacks.is_stopping = [this]() { return is_stopping(); };
    callbacks.state = [this]() { return state(); };
    callbacks.copy_video_info = [this](streambridge::StreamInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!video_info_) {
            return false;
        }
        info = *video_info_;
        return true;
    };
    callbacks.is_surface_paused = [this]() {
        return surface_paused_.load(std::memory_order_acquire);
    };
    callbacks.on_error = [this](const std::string& message) {
        set_error(message);
    };

    auto worker = std::make_unique<VideoDecodeWorker>(
        video_packet_queue_,
        audio_packet_queue_,
        first_video_pts_us_,
        clock_,
        sync_controller_,
        renderer_,
        metrics_,
        decode_path,
        std::move(callbacks));
    VideoDecodeWorker* worker_ptr = worker.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_worker_ = std::move(worker);
    }
    worker_ptr->run();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (video_worker_.get() == worker_ptr) {
            video_worker_.reset();
        }
    }
}

// ============================================================
// Audio thread: AAC decode + AAudio output + clock update
// ============================================================

void NativePlaybackSession::audio_loop() {
    AudioDecodeWorker::Callbacks callbacks;
    callbacks.is_stopping = [this]() { return is_stopping(); };
    callbacks.state = [this]() { return state(); };
    callbacks.copy_audio_info = [this](streambridge::StreamInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!audio_info_) {
            return false;
        }
        info = *audio_info_;
        return true;
    };
    callbacks.on_error = [this](const std::string& message) {
        set_error(message);
    };

    auto worker = std::make_unique<AudioDecodeWorker>(
        audio_packet_queue_,
        first_audio_pts_us_,
        clock_,
        metrics_,
        std::move(callbacks));
    AudioDecodeWorker* worker_ptr = worker.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_worker_ = std::move(worker);
    }
    worker_ptr->run();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_worker_.get() == worker_ptr) {
            audio_worker_.reset();
        }
    }
}

}  // namespace streambridge::android
