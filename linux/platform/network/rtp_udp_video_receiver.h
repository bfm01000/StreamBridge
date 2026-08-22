#pragma once
// Linux RTP/UDP video receiver: UDP datagram -> RTP packet -> H.264 Annex-B MediaPacket.

#include <cstddef>
#include <cstdint>

#include "network/udp_socket.h"
#include "streambridge/h264_rtp_depacketizer.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::linux_platform {

struct RtpUdpVideoReceiverConfig {
    uint16_t local_port = 0;
    uint8_t payload_type = kRtpPayloadTypeH264Dynamic;
    size_t max_datagram_size = 1500;
    size_t max_reorder_packets = 16;
};

struct RtpUdpVideoReceiverStats {
    uint64_t udp_datagrams = 0;
    uint64_t malformed_datagrams = 0;
    H264RtpDepacketizerStats depacketizer;
};

class RtpUdpVideoReceiver {
public:
    RtpUdpVideoReceiver() = default;
    ~RtpUdpVideoReceiver();

    RtpUdpVideoReceiver(const RtpUdpVideoReceiver&) = delete;
    RtpUdpVideoReceiver& operator=(const RtpUdpVideoReceiver&) = delete;

    Result<void> open(const RtpUdpVideoReceiverConfig& config);
    Result<MediaPacket> read_frame();
    void close();
    void interrupt();
    bool is_open() const { return socket_.is_open(); }
    uint16_t local_port() const { return socket_.local_port(); }
    RtpUdpVideoReceiverStats stats() const;

private:
    RtpUdpVideoReceiverConfig config_;
    UdpSocket socket_;
    H264RtpDepacketizer depacketizer_;
    uint64_t udp_datagrams_ = 0;
    uint64_t malformed_datagrams_ = 0;
};

}  // namespace streambridge::linux_platform
