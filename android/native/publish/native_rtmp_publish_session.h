#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "native_audio_aac_encoder.h"
#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"
#include "streambridge/publish_timestamp_aligner.h"
#include "streambridge/transport.h"

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
    int start_av(const std::string& url,
                 int width,
                 int height,
                 int frame_rate,
                 int video_bitrate_bps,
                 const std::vector<uint8_t>& video_codec_config,
                 int sample_rate,
                 int channels,
                 int audio_bitrate_bps,
                 const std::vector<uint8_t>& audio_codec_config);
    int start_rtp_video_only(const std::string& remote_host,
                             int remote_port,
                             int local_port,
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
    int write_audio_packet(const uint8_t* data,
                           size_t size,
                           int64_t pts_us,
                           int64_t duration_us);
    int start_audio_capture();
    void stop_audio_capture();
    std::vector<uint8_t> audio_codec_config() const;
    void stop();
    std::string status_text() const;

private:
    static MediaQueue<MediaPacket>::Config queue_config();
    void writer_loop();
    void on_native_audio_config(std::vector<uint8_t> config);
    void on_native_audio_packet(NativeAudioAacEncoder::EncodedPacket packet);
    void on_native_audio_error(const std::string& message);

    mutable std::mutex mutex_;
    std::unique_ptr<IMediaPublisher> publisher_;
    MediaQueue<MediaPacket> packet_queue_;
    std::unique_ptr<NativeAudioAacEncoder> audio_encoder_;
    std::thread writer_thread_;
    bool running_ = false;
    int64_t queued_count_ = 0;
    int64_t written_count_ = 0;
    int64_t key_count_ = 0;
    int64_t dropped_before_align_ = 0;
    PublishTimestampAligner timestamp_aligner_;
    std::string status_ = "PublishIdle";
};

}  // namespace streambridge::android
