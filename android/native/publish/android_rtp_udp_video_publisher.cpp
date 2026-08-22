#include "android_rtp_udp_video_publisher.h"

#include "streambridge/rtp_packet.h"

#include <utility>

namespace streambridge::android {

AndroidRtpUdpVideoPublisher::AndroidRtpUdpVideoPublisher(RtpUdpVideoTransportConfig config)
    : config_(std::move(config)), packetizer_([this] {
          H264RtpPacketizerConfig pc;
          pc.max_payload_size = config_.max_payload_size;
          pc.payload_type = static_cast<uint8_t>(config_.payload_type);
          pc.ssrc = config_.ssrc;
          return pc;
      }()) {}

AndroidRtpUdpVideoPublisher::~AndroidRtpUdpVideoPublisher() {
    close();
}

Result<void> AndroidRtpUdpVideoPublisher::open(const PublishConfig&) {
    if (config_.remote_host.empty() || config_.remote_port == 0) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "Android RTP publisher requires remote host and port");
    }
    if (config_.payload_type > 127 || config_.max_payload_size < 3) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "invalid Android RTP publisher payload config");
    }
    stats_ = {};
    return sender_.open(config_.local_port);
}

Result<void> AndroidRtpUdpVideoPublisher::write_header(const StreamInfo& video_stream,
                                                       const StreamInfo& audio_stream) {
    if (video_stream.codec != CodecId::H264) {
        return Result<void>::err(
            ErrorDomain::Codec,
            ErrorCode::CodecFormatUnsupported,
            "Android RTP publisher supports only H.264 video");
    }
    if (audio_stream.type != MediaType::Unknown) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "Android RTP publisher is video-only in this stage");
    }
    if (!video_stream.codec_extradata.empty()) {
        MediaPacket config_packet;
        config_packet.type = MediaType::Video;
        config_packet.codec = CodecId::H264;
        config_packet.h264_format = H264PacketFormat::Unknown;
        config_packet.pts.us = 0;
        config_packet.dts.us = 0;
        config_packet.is_key_frame = true;
        config_packet.data = video_stream.codec_extradata;
        return write_packet(config_packet);
    }
    return Result<void>::ok();
}

Result<void> AndroidRtpUdpVideoPublisher::write_packet(const MediaPacket& packet) {
    if (!is_open()) {
        return Result<void>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Android RTP publisher is not open");
    }
    if (packet.is_audio()) {
        return Result<void>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig, "Android RTP publisher got audio packet");
    }
    auto rtp_packets = packetizer_.packetize(packet);
    if (rtp_packets.is_err()) {
        return Result<void>::err(rtp_packets.error_domain(),
                                 rtp_packets.error_code(),
                                 rtp_packets.error_message());
    }
    for (const auto& rtp_packet : *rtp_packets) {
        auto wire = serialize_rtp_packet(rtp_packet);
        if (wire.is_err()) {
            return Result<void>::err(wire.error_domain(), wire.error_code(), wire.error_message());
        }
        auto sent = sender_.send_to(wire->data(), wire->size(), config_.remote_host, config_.remote_port);
        if (sent.is_err()) {
            return Result<void>::err(sent.error_domain(), sent.error_code(), sent.error_message());
        }
        stats_.bytes_written += static_cast<int64_t>(*sent);
        ++stats_.packets_written;
    }
    return Result<void>::ok();
}

void AndroidRtpUdpVideoPublisher::close() {
    sender_.close();
}

void AndroidRtpUdpVideoPublisher::interrupt() {
    close();
}

bool AndroidRtpUdpVideoPublisher::is_open() const {
    return sender_.is_open();
}

IMediaPublisher::Stats AndroidRtpUdpVideoPublisher::stats() const {
    return stats_;
}

}  // namespace streambridge::android

