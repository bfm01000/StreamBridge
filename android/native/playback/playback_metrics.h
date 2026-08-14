#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "streambridge/av_sync.h"
#include "streambridge/media_types.h"
#include "streambridge/session.h"

namespace streambridge::android {

// 播放链路运行指标统计：记录喂包/解码/渲染/丢帧数量与 A/V 同步差值，供状态展示和诊断使用。
class PlaybackMetrics {
public:
    void reset();

    void on_video_packet_fed();
    void on_video_frame_decoded();
    void on_video_frame_presented();
    void on_video_frame_dropped();
    void on_audio_frame_output();
    void on_packet_demuxed(const streambridge::MediaPacket& packet);
    void set_sync(streambridge::VideoSyncAction action, int64_t av_diff_us);
    void set_video_path(std::string path);
    void set_render_path(std::string path);

    int64_t video_packets_fed() const;
    int64_t video_frames_decoded() const;
    int64_t video_frames_presented() const;
    int64_t video_frames_dropped() const;
    int64_t audio_frames_output() const;
    int64_t last_av_diff_us() const;
    int64_t latest_media_pts_us() const;
    streambridge::VideoSyncAction last_sync_action() const;

    std::string status_suffix(int64_t master_clock_us) const;

private:
    double elapsed_seconds_locked() const;

    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point start_time_;
    int64_t bytes_demuxed_ = 0;
    int64_t video_packets_demuxed_ = 0;
    int64_t audio_packets_demuxed_ = 0;
    int64_t latest_video_pts_us_ = -1;
    int64_t latest_audio_pts_us_ = -1;
    int64_t video_packets_fed_ = 0;
    int64_t video_frames_decoded_ = 0;
    int64_t video_frames_presented_ = 0;
    int64_t video_frames_dropped_ = 0;
    int64_t audio_frames_output_ = 0;
    int64_t last_av_diff_us_ = 0;
    streambridge::VideoSyncAction last_sync_action_ = streambridge::VideoSyncAction::Render;
    std::string video_path_ = "unknown";
    std::string render_path_ = "unknown";
};

}  // namespace streambridge::android
