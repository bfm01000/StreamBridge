#pragma once
// Linux RTP/UDP video publisher. Implements IMediaPublisher without exposing UDP/RTP details upward.

#include <cstdint>
#include <memory>

#include "network/udp_socket.h"
#include "streambridge/h264_rtp_packetizer.h"
#include "streambridge/transport.h"
#include "streambridge/transport_config.h"

namespace streambridge {

class RtpUdpVideoPublisher final : public IMediaPublisher {
public:
    explicit RtpUdpVideoPublisher(RtpUdpVideoTransportConfig config);
    ~RtpUdpVideoPublisher() override;

    Result<void> open(const PublishConfig& config) override;
    Result<void> write_header(const StreamInfo& video_stream,
                              const StreamInfo& audio_stream) override;
    Result<void> write_packet(const MediaPacket& packet) override;
    void close() override;
    void interrupt() override;
    bool is_open() const override;
    Stats stats() const override;

private:
    RtpUdpVideoTransportConfig config_;
    linux_platform::UdpSocket socket_;
    std::unique_ptr<H264RtpPacketizer> packetizer_;
    Stats stats_;
};

}  // namespace streambridge
