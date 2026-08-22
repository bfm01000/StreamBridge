#pragma once
// Android RTP/UDP video receiver: UDP datagram -> RTP packet -> H.264 Annex-B MediaPacket.

#include <cstddef>
#include <cstdint>

#include "android_udp_receiver.h"
#include "streambridge/h264_rtp_depacketizer.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

struct AndroidRtpUdpVideoReceiverConfig {
    uint16_t local_port = 0;
    uint8_t payload_type = kRtpPayloadTypeH264Dynamic;
    size_t max_datagram_size = 1500;
    size_t max_reorder_packets = 16;
};

struct AndroidRtpUdpVideoReceiverStats {
    uint64_t udp_datagrams = 0;
    uint64_t malformed_datagrams = 0;
    H264RtpDepacketizerStats depacketizer;
};

class AndroidRtpUdpVideoReceiver {
public:
    AndroidRtpUdpVideoReceiver() = default;
    ~AndroidRtpUdpVideoReceiver();

    AndroidRtpUdpVideoReceiver(const AndroidRtpUdpVideoReceiver&) = delete;
    AndroidRtpUdpVideoReceiver& operator=(const AndroidRtpUdpVideoReceiver&) = delete;

    Result<void> open(const AndroidRtpUdpVideoReceiverConfig& config);
    Result<MediaPacket> read_frame();
    void close();
    void interrupt();
    bool is_open() const { return socket_.is_open(); }
    uint16_t local_port() const { return socket_.local_port(); }
    AndroidRtpUdpVideoReceiverStats stats() const;

private:
    AndroidRtpUdpVideoReceiverConfig config_;
    AndroidUdpReceiver socket_;
    H264RtpDepacketizer depacketizer_;
    uint64_t udp_datagrams_ = 0;
    uint64_t malformed_datagrams_ = 0;
};

}  // namespace streambridge::android
