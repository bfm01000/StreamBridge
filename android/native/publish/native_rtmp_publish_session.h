#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"
#include "ffmpeg_rtmp_publisher.h"

namespace streambridge::android {

class NativeRtmpPublishSession {
public:
    NativeRtmpPublishSession();
    ~NativeRtmpPublishSession();

    int start_video_only(const std::string& url,
                         int width,
                         int height,
                         int frame_rate,
                         int bitrate_bps,
                         const std::vector<uint8_t>& codec_config);
    int write_video_packet(const uint8_t* data,
                           size_t size,
                           int64_t pts_us,
                           int64_t dts_us,
                           int64_t duration_us,
                           bool key_frame);
    void stop();
    std::string status_text() const;

private:
    static MediaQueue<MediaPacket>::Config queue_config();
    void writer_loop();

    mutable std::mutex mutex_;
    FFmpegRTMPPublisher publisher_;
    MediaQueue<MediaPacket> packet_queue_;
    std::thread writer_thread_;
    bool running_ = false;
    int64_t queued_count_ = 0;
    int64_t written_count_ = 0;
    int64_t key_count_ = 0;
    int64_t first_pts_us_ = -1;
    std::string status_ = "PublishIdle";
};

}  // namespace streambridge::android
