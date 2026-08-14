#include "native_rtmp_publish_session.h"

#include <sstream>

#include "streambridge/logging.h"

namespace streambridge::android {

namespace {
constexpr const char* kTag = "NativeRtmpPublishSession";
}

NativeRtmpPublishSession::NativeRtmpPublishSession() = default;

NativeRtmpPublishSession::~NativeRtmpPublishSession() {
    stop();
}

int NativeRtmpPublishSession::start_video_only(
        const std::string& url,
        int width,
        int height,
        int frame_rate,
        int bitrate_bps,
        const std::vector<uint8_t>& codec_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        status_ = "PublishError already running";
        return -1;
    }
    if (url.empty() || width <= 0 || height <= 0 || codec_config.empty()) {
        status_ = "PublishError invalid video publish config";
        return -2;
    }

    PublishConfig publish_config;
    publish_config.url = url;
    auto open_result = publisher_.open(publish_config);
    if (!open_result.is_ok()) {
        status_ = "PublishError open: " + open_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        return -3;
    }

    StreamInfo video;
    video.type = MediaType::Video;
    video.codec = CodecId::H264;
    video.width = width;
    video.height = height;
    video.frame_rate = static_cast<double>(frame_rate);
    video.pixel_format = PixelFormat::Unknown;
    video.codec_extradata = codec_config;
    video.time_base = Rational::micros();
    video.bitrate_bps = bitrate_bps;

    StreamInfo no_audio;
    auto header_result = publisher_.write_header(video, no_audio);
    if (!header_result.is_ok()) {
        status_ = "PublishError header: " + header_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        publisher_.close();
        return -4;
    }

    running_ = true;
    packet_count_ = 0;
    first_pts_us_ = -1;
    status_ = "Publishing packets=0";
    SB_LOG_I(kTag, "started Android camera RTMP publish url=%s %dx%d fps=%d bitrate=%d",
             url.c_str(), width, height, frame_rate, bitrate_bps);
    return 0;
}

int NativeRtmpPublishSession::write_video_packet(
        const uint8_t* data,
        size_t size,
        int64_t pts_us,
        int64_t dts_us,
        int64_t duration_us,
        bool key_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return -1;
    }
    if (data == nullptr || size == 0) {
        return 0;
    }
    if (first_pts_us_ < 0) {
        first_pts_us_ = pts_us;
    }

    MediaPacket packet;
    packet.type = MediaType::Video;
    packet.codec = CodecId::H264;
    packet.pts.us = pts_us - first_pts_us_;
    packet.dts.us = dts_us >= 0 ? dts_us - first_pts_us_ : packet.pts.us;
    packet.duration.us = duration_us;
    packet.is_key_frame = key_frame;
    packet.data.assign(data, data + size);
    packet.sequence_number = packet_count_;

    auto result = publisher_.write_packet(packet);
    if (!result.is_ok()) {
        status_ = "PublishError packet: " + result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        publisher_.close();
        running_ = false;
        return -2;
    }

    packet_count_++;
    auto stats = publisher_.stats();
    std::ostringstream oss;
    oss << "Publishing packets=" << packet_count_
        << " bytes=" << stats.bytes_written
        << " key=" << (key_frame ? 1 : 0);
    status_ = oss.str();
    return 0;
}

void NativeRtmpPublishSession::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        SB_LOG_I(kTag, "stopping Android camera RTMP publish packets=%lld",
                 static_cast<long long>(packet_count_));
    }
    publisher_.interrupt();
    publisher_.close();
    running_ = false;
    status_ = "PublishStopped";
}

std::string NativeRtmpPublishSession::status_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

}  // namespace streambridge::android
