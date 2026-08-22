#pragma once
// Android RTP/UDP video publisher. Sender-only for current H.264 RTP stage.

#include "android_udp_sender.h"
#include "streambridge/h264_rtp_packetizer.h"
#include "streambridge/transport.h"
#include "streambridge/transport_config.h"

namespace streambridge::android {

class AndroidRtpUdpVideoPublisher final : public IMediaPublisher {
public:
    explicit AndroidRtpUdpVideoPublisher(RtpUdpVideoTransportConfig config);
    ~AndroidRtpUdpVideoPublisher() override;

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
    AndroidUdpSender sender_;
    H264RtpPacketizer packetizer_;
    Stats stats_;
};

}  // namespace streambridge::android
