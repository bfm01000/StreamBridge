#pragma once
// H.264 RTP packetizer (RFC 6184 subset): Single NAL Unit + FU-A.
// Platform independent: no socket, thread, FFmpeg, JNI, ALSA, or V4L2 types.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "streambridge/h264_nalu_parser.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/rtp_packet.h"

namespace streambridge {

constexpr uint8_t kH264NalTypeMask = 0x1F;
constexpr uint8_t kH264NalNriMask = 0x60;
constexpr uint8_t kH264NalFBitMask = 0x80;
constexpr uint8_t kH264NalTypeFuA = 28;

struct H264RtpPacketizerConfig {
    size_t max_payload_size = 1200;
    uint8_t payload_type = kRtpPayloadTypeH264Dynamic;
    uint32_t ssrc = 0;
    uint16_t initial_sequence_number = 0;
    uint32_t base_timestamp = 0;
    bool repeat_parameter_sets_before_idr = true;
};

struct H264RtpPacketizerStats {
    uint64_t frames = 0;
    uint64_t packets = 0;
    uint64_t single_nal_packets = 0;
    uint64_t fua_packets = 0;
    uint64_t sps_pps_resend_packets = 0;
    uint64_t malformed_frames = 0;
};

class H264RtpPacketizer {
public:
    explicit H264RtpPacketizer(H264RtpPacketizerConfig config)
        : config_(config), next_sequence_number_(config.initial_sequence_number) {}

    Result<std::vector<RtpPacket>> packetize(const MediaPacket& packet) {
        if (!packet.is_video() || packet.codec != CodecId::H264) {
            return Result<std::vector<RtpPacket>>::err(
                ErrorDomain::Codec,
                ErrorCode::CodecFormatUnsupported,
                "H264RtpPacketizer only accepts H.264 video packets");
        }
        if (packet.data.empty()) {
            ++stats_.malformed_frames;
            return Result<std::vector<RtpPacket>>::err(
                ErrorDomain::Codec, ErrorCode::CodecEncodeFailed, "empty H.264 packet");
        }
        if (config_.max_payload_size < 3) {
            return Result<std::vector<RtpPacket>>::err(
                ErrorDomain::Config,
                ErrorCode::InvalidConfig,
                "H.264 RTP max_payload_size must be at least 3 bytes");
        }

        H264PacketFormat format = packet.h264_format;
        int avcc_length_size = 0;
        if (format == H264PacketFormat::Unknown) {
            format = h264_detect_packet_format(packet.data.data(), packet.data.size(),
                                               &avcc_length_size);
        }
        if (format == H264PacketFormat::Unknown) {
            ++stats_.malformed_frames;
            return Result<std::vector<RtpPacket>>::err(
                ErrorDomain::Codec,
                ErrorCode::CodecFormatUnsupported,
                "unknown H.264 packet format");
        }

        auto parsed = h264_parse_nalus(packet.data.data(), packet.data.size(),
                                       format, avcc_length_size);
        if (!parsed.ok) {
            ++stats_.malformed_frames;
            return Result<std::vector<RtpPacket>>::err(
                ErrorDomain::Codec, ErrorCode::MalformedAvcc, parsed.error);
        }

        std::vector<std::vector<uint8_t>> resend_parameter_sets;
        const bool frame_has_sps = contains_nal_type(parsed.nalus, 7);
        const bool frame_has_pps = contains_nal_type(parsed.nalus, 8);
        const bool frame_has_idr = contains_nal_type(parsed.nalus, 5);
        update_parameter_set_cache(parsed.nalus);

        if (config_.repeat_parameter_sets_before_idr && frame_has_idr &&
            !(frame_has_sps && frame_has_pps)) {
            if (!cached_sps_.empty()) {
                resend_parameter_sets.push_back(cached_sps_);
            }
            if (!cached_pps_.empty()) {
                resend_parameter_sets.push_back(cached_pps_);
            }
        }

        const uint32_t timestamp = rtp_timestamp_from_pts_us(packet.pts.us,
                                                            config_.base_timestamp);
        const size_t total_nalus = resend_parameter_sets.size() + parsed.nalus.size();
        std::vector<RtpPacket> out;
        out.reserve(total_nalus);

        size_t nalu_index = 0;
        for (const auto& parameter_set : resend_parameter_sets) {
            H264NaluView view{parameter_set.data(), parameter_set.size()};
            auto status = append_nalu(view, timestamp, nalu_index + 1 == total_nalus, &out);
            if (status.is_err()) {
                return Result<std::vector<RtpPacket>>::err(
                    status.error_domain(), status.error_code(), status.error_message());
            }
            ++stats_.sps_pps_resend_packets;
            ++nalu_index;
        }

        for (const auto& nalu : parsed.nalus) {
            auto status = append_nalu(nalu, timestamp, nalu_index + 1 == total_nalus, &out);
            if (status.is_err()) {
                return Result<std::vector<RtpPacket>>::err(
                    status.error_domain(), status.error_code(), status.error_message());
            }
            ++nalu_index;
        }

        ++stats_.frames;
        stats_.packets += out.size();
        return Result<std::vector<RtpPacket>>::ok(std::move(out));
    }

    const H264RtpPacketizerStats& stats() const { return stats_; }
    uint16_t next_sequence_number() const { return next_sequence_number_; }

private:
    static bool contains_nal_type(const std::vector<H264NaluView>& nalus, uint8_t nal_type) {
        return std::any_of(nalus.begin(), nalus.end(), [nal_type](const H264NaluView& nalu) {
            return nalu.nal_type() == nal_type;
        });
    }

    void update_parameter_set_cache(const std::vector<H264NaluView>& nalus) {
        for (const auto& nalu : nalus) {
            if (nalu.is_sps()) {
                cached_sps_.assign(nalu.data, nalu.data + nalu.size);
            } else if (nalu.is_pps()) {
                cached_pps_.assign(nalu.data, nalu.data + nalu.size);
            }
        }
    }

    Result<void> append_nalu(const H264NaluView& nalu, uint32_t timestamp,
                             bool is_last_nalu, std::vector<RtpPacket>* out) {
        if (nalu.empty()) {
            ++stats_.malformed_frames;
            return Result<void>::err(
                ErrorDomain::Codec, ErrorCode::CodecEncodeFailed, "empty H.264 NAL unit");
        }
        if (nalu.size <= config_.max_payload_size) {
            RtpPacket packet;
            fill_header(is_last_nalu, timestamp, &packet.header);
            packet.payload.assign(nalu.data, nalu.data + nalu.size);
            out->push_back(std::move(packet));
            ++stats_.single_nal_packets;
            return Result<void>::ok();
        }

        return append_fua(nalu, timestamp, is_last_nalu, out);
    }

    Result<void> append_fua(const H264NaluView& nalu, uint32_t timestamp,
                            bool is_last_nalu, std::vector<RtpPacket>* out) {
        if (config_.max_payload_size <= 2) {
            return Result<void>::err(
                ErrorDomain::Config,
                ErrorCode::InvalidConfig,
                "H.264 FU-A max_payload_size must leave room for payload bytes");
        }

        const uint8_t nal_header = nalu.data[0];
        const uint8_t fu_indicator = static_cast<uint8_t>(
            (nal_header & (kH264NalFBitMask | kH264NalNriMask)) | kH264NalTypeFuA);
        const uint8_t nal_type = static_cast<uint8_t>(nal_header & kH264NalTypeMask);
        const size_t max_fragment_data = config_.max_payload_size - 2;
        size_t offset = 1;
        bool first = true;

        while (offset < nalu.size) {
            const size_t fragment_size = std::min(max_fragment_data, nalu.size - offset);
            const bool last_fragment = offset + fragment_size == nalu.size;

            RtpPacket packet;
            fill_header(is_last_nalu && last_fragment, timestamp, &packet.header);
            packet.payload.resize(2 + fragment_size);
            packet.payload[0] = fu_indicator;
            packet.payload[1] = static_cast<uint8_t>(
                (first ? 0x80 : 0) | (last_fragment ? 0x40 : 0) | nal_type);
            std::memcpy(packet.payload.data() + 2, nalu.data + offset, fragment_size);
            out->push_back(std::move(packet));
            ++stats_.fua_packets;

            first = false;
            offset += fragment_size;
        }

        return Result<void>::ok();
    }

    void fill_header(bool marker, uint32_t timestamp, RtpHeader* header) {
        header->version = kRtpVersion;
        header->marker = marker;
        header->payload_type = config_.payload_type;
        header->sequence_number = next_sequence_number_++;
        header->timestamp = timestamp;
        header->ssrc = config_.ssrc;
    }

    H264RtpPacketizerConfig config_;
    uint16_t next_sequence_number_ = 0;
    std::vector<uint8_t> cached_sps_;
    std::vector<uint8_t> cached_pps_;
    H264RtpPacketizerStats stats_;
};

}  // namespace streambridge
