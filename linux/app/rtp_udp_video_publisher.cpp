#include "rtp_udp_video_publisher.h"

#include "streambridge/rtp_packet.h"

namespace streambridge {

RtpUdpVideoPublisher::RtpUdpVideoPublisher(RtpUdpVideoTransportConfig config)
    : config_(std::move(config)) {}

RtpUdpVideoPublisher::~RtpUdpVideoPublisher() {
    close();
}

Result<void> RtpUdpVideoPublisher::open(const PublishConfig&) {
    if (config_.remote_host.empty() || config_.remote_port == 0) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "RTP/UDP video publisher requires remote host and remote port");
    }
    if (config_.payload_type > 127) {
        return Result<void>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig, "RTP payload type must be 7-bit");
    }
    if (config_.max_payload_size < 3) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "RTP max payload size must be at least 3 bytes");
    }

    auto ret = socket_.open();
    if (ret.is_err()) {
        return ret;
    }
    if (config_.local_port != 0) {
        ret = socket_.bind(config_.local_port);
        if (ret.is_err()) {
            close();
            return ret;
        }
    }

    H264RtpPacketizerConfig packetizer_config;
    packetizer_config.max_payload_size = config_.max_payload_size;
    packetizer_config.payload_type = static_cast<uint8_t>(config_.payload_type);
    packetizer_config.ssrc = config_.ssrc;
    packetizer_ = std::make_unique<H264RtpPacketizer>(packetizer_config);
    stats_ = {};
    return Result<void>::ok();
}

Result<void> RtpUdpVideoPublisher::write_header(const StreamInfo& video_stream,
                                                const StreamInfo& audio_stream) {
    if (video_stream.codec != CodecId::H264) {
        return Result<void>::err(
            ErrorDomain::Codec,
            ErrorCode::CodecFormatUnsupported,
            "RTP/UDP video publisher currently supports only H.264 video");
    }
    if (audio_stream.type != MediaType::Unknown) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "RTP/UDP video publisher does not support audio in this stage");
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

Result<void> RtpUdpVideoPublisher::write_packet(const MediaPacket& packet) {
    if (!is_open() || !packetizer_) {
        return Result<void>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "RTP/UDP publisher is not open");
    }
    if (packet.is_audio()) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "RTP/UDP video publisher received an audio packet");
    }

    auto rtp_packets = packetizer_->packetize(packet);
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
        auto sent = socket_.send_to(wire->data(), wire->size(),
                                    config_.remote_host, config_.remote_port);
        if (sent.is_err()) {
            return Result<void>::err(sent.error_domain(), sent.error_code(), sent.error_message());
        }
        stats_.bytes_written += static_cast<int64_t>(*sent);
        ++stats_.packets_written;
    }

    return Result<void>::ok();
}

void RtpUdpVideoPublisher::close() {
    socket_.close();
    packetizer_.reset();
}

void RtpUdpVideoPublisher::interrupt() {
    close();
}

bool RtpUdpVideoPublisher::is_open() const {
    return socket_.is_open();
}

IMediaPublisher::Stats RtpUdpVideoPublisher::stats() const {
    return stats_;
}

}  // namespace streambridge

