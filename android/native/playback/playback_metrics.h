#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "streambridge/av_sync.h"
#include "streambridge/session.h"

namespace streambridge::android {

class PlaybackMetrics {
public:
    void reset();

    void on_video_packet_fed();
    void on_video_frame_decoded();
    void on_video_frame_presented();
    void on_video_frame_dropped();
    void on_audio_frame_output();
    void set_sync(streambridge::VideoSyncAction action, int64_t av_diff_us);

    int64_t video_packets_fed() const;
    int64_t video_frames_decoded() const;
    int64_t video_frames_presented() const;
    int64_t video_frames_dropped() const;
    int64_t audio_frames_output() const;
    int64_t last_av_diff_us() const;
    streambridge::VideoSyncAction last_sync_action() const;

    std::string status_suffix() const;

private:
    mutable std::mutex mutex_;
    int64_t video_packets_fed_ = 0;
    int64_t video_frames_decoded_ = 0;
    int64_t video_frames_presented_ = 0;
    int64_t video_frames_dropped_ = 0;
    int64_t audio_frames_output_ = 0;
    int64_t last_av_diff_us_ = 0;
    streambridge::VideoSyncAction last_sync_action_ = streambridge::VideoSyncAction::Render;
};

}  // namespace streambridge::android
