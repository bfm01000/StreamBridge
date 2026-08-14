#include "playback_metrics.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace streambridge::android {

void PlaybackMetrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    start_time_ = std::chrono::steady_clock::now();
    bytes_demuxed_ = 0;
    video_packets_demuxed_ = 0;
    audio_packets_demuxed_ = 0;
    latest_video_pts_us_ = -1;
    latest_audio_pts_us_ = -1;
    video_packets_fed_ = 0;
    video_frames_decoded_ = 0;
    video_frames_presented_ = 0;
    video_frames_dropped_ = 0;
    audio_frames_output_ = 0;
    last_av_diff_us_ = 0;
    last_sync_action_ = streambridge::VideoSyncAction::Render;
    video_path_ = "unknown";
    render_path_ = "unknown";
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

void PlaybackMetrics::on_packet_demuxed(const streambridge::MediaPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_demuxed_ += static_cast<int64_t>(packet.data.size());
    bytes_demuxed_ += static_cast<int64_t>(packet.codec_config.size());

    if (packet.type == streambridge::MediaType::Video) {
        ++video_packets_demuxed_;
        if (packet.has_valid_pts()) {
            latest_video_pts_us_ = packet.pts.us;
        }
    } else if (packet.type == streambridge::MediaType::Audio) {
        ++audio_packets_demuxed_;
        if (packet.has_valid_pts()) {
            latest_audio_pts_us_ = packet.pts.us;
        }
    }
}

void PlaybackMetrics::set_sync(streambridge::VideoSyncAction action, int64_t av_diff_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_sync_action_ = action;
    last_av_diff_us_ = av_diff_us;
}

void PlaybackMetrics::set_video_path(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    video_path_ = std::move(path);
}

void PlaybackMetrics::set_render_path(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    render_path_ = std::move(path);
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

int64_t PlaybackMetrics::latest_media_pts_us() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_video_pts_us_ >= 0 && latest_audio_pts_us_ >= 0) {
        return latest_video_pts_us_ > latest_audio_pts_us_
            ? latest_video_pts_us_
            : latest_audio_pts_us_;
    }
    return latest_video_pts_us_ >= 0 ? latest_video_pts_us_ : latest_audio_pts_us_;
}

streambridge::VideoSyncAction PlaybackMetrics::last_sync_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_sync_action_;
}

double PlaybackMetrics::elapsed_seconds_locked() const {
    const auto elapsed = std::chrono::steady_clock::now() - start_time_;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return elapsed_ms > 0 ? static_cast<double>(elapsed_ms) / 1000.0 : 0.001;
}

std::string PlaybackMetrics::status_suffix(int64_t master_clock_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const double elapsed_s = elapsed_seconds_locked();
    const int64_t bitrate_kbps =
        static_cast<int64_t>((static_cast<double>(bytes_demuxed_) * 8.0) /
                             elapsed_s / 1000.0);
    const double video_fps =
        static_cast<double>(video_frames_presented_) / elapsed_s;
    const double audio_fps =
        static_cast<double>(audio_frames_output_) / elapsed_s;
    const int64_t latest_media_pts_us =
        latest_video_pts_us_ > latest_audio_pts_us_
            ? latest_video_pts_us_
            : latest_audio_pts_us_;
    int64_t buffer_delay_ms = 0;
    if (latest_media_pts_us >= 0 && master_clock_us >= 0) {
        const int64_t delay_us = latest_media_pts_us - master_clock_us;
        buffer_delay_ms = delay_us > 0 ? delay_us / 1000 : 0;
    }

    std::ostringstream oss;
    oss << " vid=" << video_frames_presented_
        << " drop=" << video_frames_dropped_
        << " aud=" << audio_frames_output_
        << " bitrate=" << bitrate_kbps << "kbps"
        << " vfps=" << std::fixed << std::setprecision(1) << video_fps
        << " afps=" << std::fixed << std::setprecision(1) << audio_fps
        << " buf_delay=" << buffer_delay_ms << "ms"
        << " sync=" << streambridge::video_sync_action_name(last_sync_action_)
        << " av_diff=" << last_av_diff_us_ << "us"
        << " video=" << video_path_
        << " render=" << render_path_;
    return oss.str();
}

}  // namespace streambridge::android
