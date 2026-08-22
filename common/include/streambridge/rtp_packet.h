#pragma once
// Minimal RTP packet primitives for H.264 video over UDP.
// Platform independent: no socket, thread, FFmpeg, JNI, ALSA, or V4L2 types.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

constexpr size_t kRtpFixedHeaderSize = 12;
constexpr uint8_t kRtpVersion = 2;
constexpr uint8_t kRtpPayloadTypeH264Dynamic = 96;
constexpr int kRtpVideoClockRate = 90'000;

struct RtpHeader {
    uint8_t version = kRtpVersion;
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;
    uint8_t payload_type = kRtpPayloadTypeH264Dynamic;
    uint16_t sequence_number = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
};

struct RtpPacket {
    RtpHeader header;
    std::vector<uint8_t> payload;

    size_t wire_size() const {
        return kRtpFixedHeaderSize + payload.size();
    }
};

inline uint32_t rtp_timestamp_from_pts_us(int64_t pts_us, uint32_t base_timestamp = 0) {
    if (pts_us <= 0) {
        return base_timestamp;
    }
    const uint64_t ticks = (static_cast<uint64_t>(pts_us) * kRtpVideoClockRate + 500'000ULL)
                           / 1'000'000ULL;
    return base_timestamp + static_cast<uint32_t>(ticks);
}

inline int64_t pts_us_delta_from_rtp_timestamp(uint32_t timestamp_delta) {
    return (static_cast<int64_t>(timestamp_delta) * 1'000'000LL) / kRtpVideoClockRate;
}

inline void write_u16_be(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

inline void write_u32_be(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

inline uint16_t read_u16_be(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8)
                                 | static_cast<uint16_t>(data[1]));
}

inline uint32_t read_u32_be(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24)
           | (static_cast<uint32_t>(data[1]) << 16)
           | (static_cast<uint32_t>(data[2]) << 8)
           | static_cast<uint32_t>(data[3]);
}

inline Result<std::vector<uint8_t>> serialize_rtp_packet(const RtpPacket& packet) {
    const RtpHeader& h = packet.header;
    if (h.version != kRtpVersion) {
        return Result<std::vector<uint8_t>>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig, "RTP version must be 2");
    }
    if (h.csrc_count != 0 || h.padding || h.extension) {
        return Result<std::vector<uint8_t>>::err(
            ErrorDomain::Config,
            ErrorCode::NotImplemented,
            "Only fixed 12-byte RTP headers are supported in this stage");
    }
    if (h.payload_type > 127) {
        return Result<std::vector<uint8_t>>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig, "RTP payload type must be 7-bit");
    }

    std::vector<uint8_t> wire(kRtpFixedHeaderSize + packet.payload.size());
    wire[0] = static_cast<uint8_t>((h.version << 6)
                                   | (h.padding ? 0x20 : 0)
                                   | (h.extension ? 0x10 : 0)
                                   | (h.csrc_count & 0x0F));
    wire[1] = static_cast<uint8_t>((h.marker ? 0x80 : 0) | (h.payload_type & 0x7F));
    write_u16_be(wire.data() + 2, h.sequence_number);
    write_u32_be(wire.data() + 4, h.timestamp);
    write_u32_be(wire.data() + 8, h.ssrc);
    if (!packet.payload.empty()) {
        std::memcpy(wire.data() + kRtpFixedHeaderSize,
                    packet.payload.data(),
                    packet.payload.size());
    }
    return Result<std::vector<uint8_t>>::ok(std::move(wire));
}

inline Result<RtpPacket> parse_rtp_packet(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kRtpFixedHeaderSize) {
        return Result<RtpPacket>::err(
            ErrorDomain::Network, ErrorCode::NetworkReadFailed, "RTP packet too short");
    }

    RtpPacket packet;
    packet.header.version = static_cast<uint8_t>(data[0] >> 6);
    packet.header.padding = (data[0] & 0x20) != 0;
    packet.header.extension = (data[0] & 0x10) != 0;
    packet.header.csrc_count = static_cast<uint8_t>(data[0] & 0x0F);
    packet.header.marker = (data[1] & 0x80) != 0;
    packet.header.payload_type = static_cast<uint8_t>(data[1] & 0x7F);
    packet.header.sequence_number = read_u16_be(data + 2);
    packet.header.timestamp = read_u32_be(data + 4);
    packet.header.ssrc = read_u32_be(data + 8);

    if (packet.header.version != kRtpVersion) {
        return Result<RtpPacket>::err(
            ErrorDomain::Network, ErrorCode::NetworkReadFailed, "Unsupported RTP version");
    }
    if (packet.header.csrc_count != 0 || packet.header.padding || packet.header.extension) {
        return Result<RtpPacket>::err(
            ErrorDomain::Network,
            ErrorCode::NotImplemented,
            "Only fixed 12-byte RTP headers are supported in this stage");
    }

    packet.payload.assign(data + kRtpFixedHeaderSize, data + size);
    return Result<RtpPacket>::ok(std::move(packet));
}

inline Result<RtpPacket> parse_rtp_packet(const std::vector<uint8_t>& data) {
    return parse_rtp_packet(data.data(), data.size());
}

}  // namespace streambridge
