#pragma once
// Audio-Video sync: common layer, platform-independent
// Audio clock is the master; video syncs to audio

#include <cstdint>

#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// 1. Monotonic clock (POSIX, works on Linux + Android)
// ============================================================

TimePointUs monotonic_now_us();

// ============================================================
// 2. Media clock: audio-driven master clock with wall-clock fallback
// ============================================================

class MediaClock {
public:
    void reset();

    // Start with wall clock (pre-audio phase)
    void start(TimePointUs first_media_pts_us);

    // Update from audio device position (audio thread calls this)
    // first_audio_pts_us: PTS of the first audio frame (after normalization)
    // played_frames: number of frames the audio device has consumed
    // sample_rate: audio sample rate
    void update_audio(TimePointUs first_audio_pts_us,
                      int64_t played_frames,
                      int sample_rate);

    // Current master clock time in microseconds
    // If audio clock active: returns last known audio position
    // Otherwise: returns wall clock
    TimePointUs now() const;

    bool has_audio_clock() const { return has_audio_; }

private:
    bool started_ = false;
    bool has_audio_ = false;
    TimePointUs first_pts_{0};
    TimePointUs start_mono_{0};
    TimePointUs audio_time_{0};
};

// ============================================================
// 3. AV sync: video sync actions and decisions
// ============================================================

enum class VideoSyncAction {
    Wait,        // Video is too early — sleep and retry
    Render,      // On time — render immediately
    RenderLate,  // Slightly behind — render immediately (no wait)
    Drop,        // Too far behind — skip this frame
};

const char* video_sync_action_name(VideoSyncAction action);

struct VideoSyncDecision {
    VideoSyncAction action = VideoSyncAction::Render;
    int64_t av_diff_us = 0;   // video_pts - master_clock (positive = video ahead)
    int64_t wait_us = 0;      // microseconds to sleep (only valid for Wait action)
};

// ============================================================
// 4. AV sync controller: decides what to do with each video frame
// ============================================================

class AVSyncController {
public:
    struct Config {
        // Video PTS ahead of master by more than this -> Wait
        int64_t early_threshold_us = 40'000;    // 40ms

        // Video PTS behind master by more than this -> RenderLate (no wait)
        int64_t late_threshold_us = -40'000;    // -40ms

        // Video PTS behind master by more than this -> Drop
        int64_t drop_threshold_us = -120'000;   // -120ms
    };

    AVSyncController();
    explicit AVSyncController(Config cfg);

    // Decide action for a video frame given its PTS and the master clock
    VideoSyncDecision decide(TimePointUs video_pts_us,
                             TimePointUs master_clock_us) const;

    // Allow runtime tuning
    Config& config() { return cfg_; }
    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace streambridge
