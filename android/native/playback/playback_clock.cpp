#include "playback_clock.h"

#include <algorithm>
#include <ctime>

namespace streambridge::android {

streambridge::TimePointUs monotonic_now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return streambridge::TimePointUs{
            static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000};
}

void PlaybackClock::reset() {
    started_ = false;
    has_audio_clock_ = false;
    first_media_pts_us_ = streambridge::TimePointUs{0};
    start_monotonic_us_ = streambridge::TimePointUs{0};
    last_audio_media_time_us_ = streambridge::TimePointUs{0};
}

void PlaybackClock::start_wall_clock(streambridge::TimePointUs first_media_pts_us) {
    started_ = true;
    has_audio_clock_ = false;
    first_media_pts_us_ = first_media_pts_us;
    start_monotonic_us_ = monotonic_now_us();
    last_audio_media_time_us_ = first_media_pts_us;
}

void PlaybackClock::update_audio_position(streambridge::TimePointUs first_audio_pts_us,
                                          int64_t played_frames,
                                          int sample_rate) {
    if (sample_rate <= 0 || played_frames < 0) {
        return;
    }
    started_ = true;
    has_audio_clock_ = true;
    first_media_pts_us_ = first_audio_pts_us;
    last_audio_media_time_us_ =
            first_audio_pts_us + streambridge::TimeDeltaUs::from_samples(played_frames, sample_rate);
}

streambridge::TimePointUs PlaybackClock::current_media_time_us() const {
    if (!started_) {
        return streambridge::TimePointUs{0};
    }
    if (has_audio_clock_) {
        return last_audio_media_time_us_;
    }
    return first_media_pts_us_ + (monotonic_now_us() - start_monotonic_us_);
}

VideoSyncDecision AVSyncController::decide(
        streambridge::TimePointUs video_pts_us,
        streambridge::TimePointUs master_clock_us) const {
    const int64_t diff_us = (video_pts_us - master_clock_us).us;
    VideoSyncDecision decision;
    decision.av_diff_us = diff_us;

    if (diff_us > kEarlyThresholdUs) {
        decision.action = VideoSyncAction::Wait;
        decision.wait_us = diff_us;
    } else if (diff_us < kDropThresholdUs) {
        decision.action = VideoSyncAction::Drop;
    } else if (diff_us < kLateThresholdUs) {
        decision.action = VideoSyncAction::RenderLate;
    } else {
        decision.action = VideoSyncAction::Render;
    }
    return decision;
}

const char* video_sync_action_name(VideoSyncAction action) {
    switch (action) {
        case VideoSyncAction::Wait:
            return "Wait";
        case VideoSyncAction::Render:
            return "Render";
        case VideoSyncAction::RenderLate:
            return "RenderLate";
        case VideoSyncAction::Drop:
            return "Drop";
    }
    return "Unknown";
}

}  // namespace streambridge::android
