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
