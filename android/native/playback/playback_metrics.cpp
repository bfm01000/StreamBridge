#include "playback_metrics.h"

#include <sstream>

namespace streambridge::android {

void PlaybackMetrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    video_packets_fed_ = 0;
    video_frames_decoded_ = 0;
    video_frames_presented_ = 0;
    video_frames_dropped_ = 0;
    audio_frames_output_ = 0;
    last_av_diff_us_ = 0;
    last_sync_action_ = streambridge::VideoSyncAction::Render;
}

void PlaybackMetrics::on_video_packet_fed() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++video_packets_fed_;
}

void PlaybackMetrics::on_video_frame_decoded() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++video_frames_decoded_;
}

void PlaybackMetrics::on_video_frame_presented() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++video_frames_presented_;
}

void PlaybackMetrics::on_video_frame_dropped() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++video_frames_dropped_;
}

void PlaybackMetrics::on_audio_frame_output() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++audio_frames_output_;
}

void PlaybackMetrics::set_sync(streambridge::VideoSyncAction action, int64_t av_diff_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_sync_action_ = action;
    last_av_diff_us_ = av_diff_us;
}

int64_t PlaybackMetrics::video_packets_fed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_packets_fed_;
}

int64_t PlaybackMetrics::video_frames_decoded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_frames_decoded_;
}

int64_t PlaybackMetrics::video_frames_presented() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_frames_presented_;
}

int64_t PlaybackMetrics::video_frames_dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_frames_dropped_;
}

int64_t PlaybackMetrics::audio_frames_output() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audio_frames_output_;
}

int64_t PlaybackMetrics::last_av_diff_us() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_av_diff_us_;
}

streambridge::VideoSyncAction PlaybackMetrics::last_sync_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_sync_action_;
}

std::string PlaybackMetrics::status_suffix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << " vid=" << video_frames_presented_
        << " drop=" << video_frames_dropped_
        << " aud=" << audio_frames_output_
        << " sync=" << streambridge::video_sync_action_name(last_sync_action_)
        << " av_diff=" << last_av_diff_us_ << "us";
    return oss.str();
}

}  // namespace streambridge::android
