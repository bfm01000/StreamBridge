#include "android_rtp_udp_video_receiver.h"

#include <utility>

#include "streambridge/rtp_packet.h"

namespace streambridge::android {

AndroidRtpUdpVideoReceiver::~AndroidRtpUdpVideoReceiver() {
    close();
}

Result<void> AndroidRtpUdpVideoReceiver::open(const AndroidRtpUdpVideoReceiverConfig& config) {
    if (config.max_datagram_size < kRtpFixedHeaderSize + 1) {
        return Result<void>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "Android RTP/UDP receiver max datagram size is too small");
    }
    config_ = config;
    H264RtpDepacketizerConfig depacketizer_config;
    depacketizer_config.payload_type = config.payload_type;
    depacketizer_config.max_reorder_packets = config.max_reorder_packets;
    depacketizer_ = H264RtpDepacketizer(depacketizer_config);
    udp_datagrams_ = 0;
    malformed_datagrams_ = 0;
    return socket_.bind(config.local_port);
}

Result<MediaPacket> AndroidRtpUdpVideoReceiver::read_frame() {
    if (!is_open()) {
        return Result<MediaPacket>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Android RTP/UDP receiver is not open");
    }

    while (is_open()) {
        auto datagram = socket_.recv_datagram(config_.max_datagram_size);
        if (datagram.is_err()) {
            return Result<MediaPacket>::err(
                datagram.error_domain(), datagram.error_code(), datagram.error_message());
        }
        ++udp_datagrams_;

        auto rtp = parse_rtp_packet(datagram->data);
        if (rtp.is_err()) {
            ++malformed_datagrams_;
            continue;
        }
        auto frames = depacketizer_.push_packet(*rtp);
        if (frames.is_err()) {
            ++malformed_datagrams_;
            continue;
        }
        if (!frames->empty()) {
            return Result<MediaPacket>::ok(std::move((*frames)[0]));
        }
    }

    return Result<MediaPacket>::err(
        ErrorDomain::Network, ErrorCode::NetworkDisconnected, "Android RTP/UDP receiver closed");
}

void AndroidRtpUdpVideoReceiver::close() {
    socket_.close();
}

void AndroidRtpUdpVideoReceiver::interrupt() {
    close();
}

AndroidRtpUdpVideoReceiverStats AndroidRtpUdpVideoReceiver::stats() const {
    AndroidRtpUdpVideoReceiverStats s;
    s.udp_datagrams = udp_datagrams_;
    s.malformed_datagrams = malformed_datagrams_;
    s.depacketizer = depacketizer_.stats();
    return s;
}

}  // namespace streambridge::android
