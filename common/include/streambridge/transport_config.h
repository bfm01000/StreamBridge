#pragma once
// Platform-independent transport selection config.

#include <cstddef>
#include <cstdint>
#include <string>

namespace streambridge {

enum class TransportKind {
    RtmpFlv = 0,
    RtpUdpVideo = 1,
};

inline const char* transport_kind_name(TransportKind kind) {
    switch (kind) {
        case TransportKind::RtmpFlv: return "RTMP/FLV";
        case TransportKind::RtpUdpVideo: return "RTP/UDP video";
    }
    return "Unknown";
}

struct RtmpFlvTransportConfig {
    std::string url;
    int connect_timeout_ms = 10'000;
    int write_timeout_ms = 5'000;
    int read_timeout_ms = 5'000;
};

struct RtpUdpVideoTransportConfig {
    std::string remote_host;
    uint16_t remote_port = 0;
    uint16_t local_port = 0;
    uint16_t payload_type = 96;
    uint32_t ssrc = 0;
    size_t max_payload_size = 1200;
};

struct TransportConfig {
    TransportKind kind = TransportKind::RtmpFlv;
    RtmpFlvTransportConfig rtmp_flv;
    RtpUdpVideoTransportConfig rtp_udp_video;

    bool is_rtmp() const { return kind == TransportKind::RtmpFlv; }
    bool is_rtp_udp_video() const { return kind == TransportKind::RtpUdpVideo; }
};

}  // namespace streambridge