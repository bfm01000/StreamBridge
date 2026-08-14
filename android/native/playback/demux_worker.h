#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "ffmpeg/ffmpeg_subscriber.h"
#include "playback_metrics.h"
#include "streambridge/av_sync.h"
#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

class DemuxWorker {
public:
    struct Callbacks {
        std::function<bool()> is_stopping;
        std::function<void(const streambridge::StreamInfo* video,
                           const streambridge::StreamInfo* audio)> on_stream_info;
        std::function<void()> on_reconnecting;
        std::function<void()> on_running;
        std::function<void(const std::string&)> on_error;
    };

    DemuxWorker(streambridge::MediaQueue<streambridge::MediaPacket>& video_queue,
                streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue,
                std::atomic<int64_t>& first_video_pts_us,
                std::atomic<int64_t>& first_audio_pts_us,
                streambridge::MediaClock& clock,
                PlaybackMetrics& metrics,
                Callbacks callbacks);

    void run(const std::string& url);
    void close();

private:
    bool is_stopping() const;
    bool push_packet(streambridge::MediaPacket packet, bool& connection_lost);

    streambridge::MediaQueue<streambridge::MediaPacket>& video_queue_;
    streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue_;
    std::atomic<int64_t>& first_video_pts_us_;
    std::atomic<int64_t>& first_audio_pts_us_;
    streambridge::MediaClock& clock_;
    PlaybackMetrics& metrics_;
    Callbacks callbacks_;
    streambridge::ffmpeg::FFmpegSubscriber subscriber_;
};

}  // namespace streambridge::android
