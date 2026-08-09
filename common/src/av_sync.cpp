#include "streambridge/av_sync.h"

#include <algorithm>
#include <ctime>

namespace streambridge {

// ============================================================
// Monotonic clock
// ============================================================

TimePointUs monotonic_now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return TimePointUs{
        static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000};
}

// ============================================================
// MediaClock
// ============================================================

void MediaClock::reset() {
    started_ = false;
    has_audio_ = false;
    first_pts_ = TimePointUs{0};
    start_mono_ = TimePointUs{0};
    audio_time_ = TimePointUs{0};
}

void MediaClock::start(TimePointUs first_media_pts_us) {
    started_ = true;
    has_audio_ = false;
    first_pts_ = first_media_pts_us;
    start_mono_ = monotonic_now_us();
    audio_time_ = first_media_pts_us;
}

void MediaClock::update_audio(TimePointUs first_audio_pts_us,
                               int64_t played_frames,
                               int sample_rate) {
    if (sample_rate <= 0 || played_frames < 0) {
        return;
    }
    started_ = true;
    has_audio_ = true;
    first_pts_ = first_audio_pts_us;
    audio_time_ = first_audio_pts_us
                  + TimeDeltaUs::from_samples(played_frames, sample_rate);
}

TimePointUs MediaClock::now() const {
    if (!started_) {
        return TimePointUs{0};
    }
    if (has_audio_) {
        // Audio clock: last known audio device position
        // NOTE: This is a sample-and-hold. Between update_audio() calls,
        // the clock does not advance. The audio thread should call
        // update_audio() frequently enough (every ~20ms for AAC frames)
        // that this is acceptable.
        return audio_time_;
    }
    // Wall clock fallback (pre-audio phase or no audio stream)
    return first_pts_ + (monotonic_now_us() - start_mono_);
}

// ============================================================
// AVSyncController
// ============================================================

AVSyncController::AVSyncController() : cfg_{} {}

AVSyncController::AVSyncController(Config cfg) : cfg_(cfg) {}

VideoSyncDecision AVSyncController::decide(
        TimePointUs video_pts_us,
        TimePointUs master_clock_us) const {
    const int64_t diff_us = (video_pts_us - master_clock_us).us;

    VideoSyncDecision decision;
    decision.av_diff_us = diff_us;

    if (diff_us > cfg_.early_threshold_us) {
        // Video ahead of audio: wait
        decision.action = VideoSyncAction::Wait;
        decision.wait_us = diff_us;
    } else if (diff_us < cfg_.drop_threshold_us) {
        // Video too far behind: drop
        decision.action = VideoSyncAction::Drop;
    } else if (diff_us < cfg_.late_threshold_us) {
        // Video slightly behind: render immediately (no wait)
        decision.action = VideoSyncAction::RenderLate;
    } else {
        // Video on time: render
        decision.action = VideoSyncAction::Render;
    }

    return decision;
}

// ============================================================
// Helper
// ============================================================

const char* video_sync_action_name(VideoSyncAction action) {
    switch (action) {
        case VideoSyncAction::Wait:       return "Wait";
        case VideoSyncAction::Render:     return "Render";
        case VideoSyncAction::RenderLate: return "RenderLate";
        case VideoSyncAction::Drop:       return "Drop";
    }
    return "Unknown";
}

}  // namespace streambridge
