#include "publisher_factory.h"

#include "ffmpeg/ffmpeg_rtmp_publisher.h"
#include "rtp_udp_video_publisher.h"

namespace streambridge {

Result<std::unique_ptr<IMediaPublisher>> create_publisher_for_transport(
    const TransportConfig& config) {
    switch (config.kind) {
        case TransportKind::RtmpFlv:
            return Result<std::unique_ptr<IMediaPublisher>>::ok(
                std::make_unique<FFmpegRTMPPublisher>());
        case TransportKind::RtpUdpVideo:
            return Result<std::unique_ptr<IMediaPublisher>>::ok(
                std::make_unique<RtpUdpVideoPublisher>(config.rtp_udp_video));
    }
    return Result<std::unique_ptr<IMediaPublisher>>::err(
        ErrorDomain::Config,
        ErrorCode::InvalidConfig,
        "unknown transport kind");
}

}  // namespace streambridge
