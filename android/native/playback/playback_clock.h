#pragma once

#include <cstdint>

#include "streambridge/media_types.h"

namespace streambridge::android {

streambridge::TimePointUs monotonic_now_us();

class PlaybackClock {
public:
    void reset();
    void start_wall_clock(streambridge::TimePointUs first_media_pts_us);
    void update_audio_position(streambridge::TimePointUs first_audio_pts_us,
                               int64_t played_frames,
                               int sample_rate);

    streambridge::TimePointUs current_media_time_us() const;
    bool has_audio_clock() const { return has_audio_clock_; }

private:
    bool started_ = false;
    bool has_audio_clock_ = false;
    streambridge::TimePointUs first_media_pts_us_{0};
    streambridge::TimePointUs start_monotonic_us_{0};
    streambridge::TimePointUs last_audio_media_time_us_{0};
};

enum class VideoSyncAction {
    Wait,
    Render,
    RenderLate,
    Drop,
};

struct VideoSyncDecision {
    VideoSyncAction action = VideoSyncAction::Render;
    int64_t av_diff_us = 0;
    int64_t wait_us = 0;
};

class AVSyncController {
public:
    VideoSyncDecision decide(streambridge::TimePointUs video_pts_us,
                             streambridge::TimePointUs master_clock_us) const;

private:
    static constexpr int64_t kEarlyThresholdUs = 40'000;
    static constexpr int64_t kLateThresholdUs = -40'000;
    static constexpr int64_t kDropThresholdUs = -120'000;
};

const char* video_sync_action_name(VideoSyncAction action);

}  // namespace streambridge::android
