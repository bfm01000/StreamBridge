#pragma once
// H.264 RTP depacketizer (RFC 6184 subset): Single NAL Unit + FU-A.
// Produces Annex-B access units as MediaPacket. Platform independent.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "streambridge/h264_rtp_packetizer.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/rtp_packet.h"

namespace streambridge {

struct H264RtpDepacketizerConfig {
    uint8_t payload_type = kRtpPayloadTypeH264Dynamic;
    size_t max_reorder_packets = 16;
};

struct H264RtpDepacketizerStats {
    uint64_t received_packets = 0;
    uint64_t output_frames = 0;
    uint64_t lost_packets = 0;
    uint64_t reordered_packets = 0;
    uint64_t duplicate_packets = 0;
    uint64_t dropped_frames = 0;
    uint64_t malformed_packets = 0;
};

class H264RtpDepacketizer {
public:
    explicit H264RtpDepacketizer(H264RtpDepacketizerConfig config = {})
        : config_(config) {}

    Result<std::vector<MediaPacket>> push_packet(const RtpPacket& packet) {
        if (packet.header.payload_type != config_.payload_type) {
            ++stats_.malformed_packets;
            return Result<std::vector<MediaPacket>>::err(
                ErrorDomain::Network,
                ErrorCode::NetworkReadFailed,
                "unexpected RTP payload type for H.264 depacketizer");
        }
        if (packet.payload.empty()) {
            ++stats_.malformed_packets;
            return Result<std::vector<MediaPacket>>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, "empty RTP payload");
        }

        ++stats_.received_packets;
        if (!expected_sequence_initialized_) {
            expected_sequence_number_ = packet.header.sequence_number;
            expected_sequence_initialized_ = true;
        } else if (packet.header.sequence_number != expected_sequence_number_) {
            ++stats_.reordered_packets;
        }

        const auto inserted = reorder_buffer_.emplace(packet.header.sequence_number, packet);
        if (!inserted.second) {
            ++stats_.duplicate_packets;
            return Result<std::vector<MediaPacket>>::ok({});
        }

        std::vector<MediaPacket> frames;
        auto status = drain_ordered_packets(&frames);
        if (status.is_err()) {
            return Result<std::vector<MediaPacket>>::err(
                status.error_domain(), status.error_code(), status.error_message());
        }
        return Result<std::vector<MediaPacket>>::ok(std::move(frames));
    }

    void reset() {
        reorder_buffer_.clear();
        current_access_unit_.clear();
        current_fragment_.clear();
        expected_sequence_initialized_ = false;
        base_timestamp_initialized_ = false;
        assembling_fragment_ = false;
        current_frame_timestamp_ = 0;
    }

    const H264RtpDepacketizerStats& stats() const { return stats_; }

private:
    Result<void> drain_ordered_packets(std::vector<MediaPacket>* frames) {
        while (true) {
            auto it = reorder_buffer_.find(expected_sequence_number_);
            if (it != reorder_buffer_.end()) {
                const RtpPacket packet = std::move(it->second);
                reorder_buffer_.erase(it);
                auto status = process_ordered_packet(packet, frames);
                ++expected_sequence_number_;
                if (status.is_err()) {
                    return status;
                }
                continue;
            }

            if (reorder_buffer_.size() > config_.max_reorder_packets) {
                ++stats_.lost_packets;
                drop_current_frame();
                ++expected_sequence_number_;
                continue;
            }
            break;
        }
        return Result<void>::ok();
    }

    Result<void> process_ordered_packet(const RtpPacket& packet,
                                        std::vector<MediaPacket>* frames) {
        const uint8_t nal_header = packet.payload[0];
        const uint8_t nal_type = static_cast<uint8_t>(nal_header & kH264NalTypeMask);
        current_frame_timestamp_ = packet.header.timestamp;

        if (nal_type >= 1 && nal_type <= 23) {
            append_annexb_nalu(packet.payload.data(), packet.payload.size());
            if (packet.header.marker) {
                finish_current_frame(packet.header.timestamp, frames);
            }
            return Result<void>::ok();
        }

        if (nal_type == kH264NalTypeFuA) {
            return process_fua_packet(packet, frames);
        }

        ++stats_.malformed_packets;
        return Result<void>::err(
            ErrorDomain::Network,
            ErrorCode::NotImplemented,
            "unsupported H.264 RTP aggregation or packetization mode");
    }

    Result<void> process_fua_packet(const RtpPacket& packet,
                                    std::vector<MediaPacket>* frames) {
        if (packet.payload.size() < 3) {
            ++stats_.malformed_packets;
            drop_current_frame();
            return Result<void>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, "malformed FU-A payload");
        }

        const uint8_t fu_indicator = packet.payload[0];
        const uint8_t fu_header = packet.payload[1];
        const bool start = (fu_header & 0x80) != 0;
        const bool end = (fu_header & 0x40) != 0;
        const uint8_t original_type = static_cast<uint8_t>(fu_header & kH264NalTypeMask);
        const uint8_t reconstructed_header = static_cast<uint8_t>(
            (fu_indicator & (kH264NalFBitMask | kH264NalNriMask)) | original_type);

        if (start) {
            if (assembling_fragment_) {
                drop_current_frame();
            }
            current_fragment_.clear();
            current_fragment_.push_back(reconstructed_header);
            current_fragment_.insert(current_fragment_.end(),
                                     packet.payload.begin() + 2,
                                     packet.payload.end());
            assembling_fragment_ = true;
        } else {
            if (!assembling_fragment_) {
                ++stats_.malformed_packets;
                return Result<void>::ok();
            }
            current_fragment_.insert(current_fragment_.end(),
                                     packet.payload.begin() + 2,
                                     packet.payload.end());
        }

        if (end) {
            if (!assembling_fragment_ || current_fragment_.empty()) {
                ++stats_.malformed_packets;
                return Result<void>::ok();
            }
            append_annexb_nalu(current_fragment_.data(), current_fragment_.size());
            current_fragment_.clear();
            assembling_fragment_ = false;
            if (packet.header.marker) {
                finish_current_frame(packet.header.timestamp, frames);
            }
        }

        return Result<void>::ok();
    }

    void append_annexb_nalu(const uint8_t* data, size_t size) {
        static constexpr uint8_t kStartCode[] = {0, 0, 0, 1};
        current_access_unit_.insert(current_access_unit_.end(),
                                    kStartCode,
                                    kStartCode + sizeof(kStartCode));
        current_access_unit_.insert(current_access_unit_.end(), data, data + size);
    }

    void finish_current_frame(uint32_t rtp_timestamp, std::vector<MediaPacket>* frames) {
        if (current_access_unit_.empty() || assembling_fragment_) {
            drop_current_frame();
            return;
        }
        if (!base_timestamp_initialized_) {
            base_timestamp_ = rtp_timestamp;
            base_timestamp_initialized_ = true;
        }

        MediaPacket packet;
        packet.type = MediaType::Video;
        packet.codec = CodecId::H264;
        packet.h264_format = H264PacketFormat::AnnexB;
        packet.pts.us = pts_us_delta_from_rtp_timestamp(rtp_timestamp - base_timestamp_);
        packet.dts = packet.pts;
        packet.data = std::move(current_access_unit_);
        const auto parsed = h264_parse_annexb_nalus(packet.data.data(), packet.data.size());
        for (const auto& nalu : parsed.nalus) {
            if (nalu.is_idr()) {
                packet.is_key_frame = true;
                break;
            }
        }
        frames->push_back(std::move(packet));
        ++stats_.output_frames;
        current_access_unit_.clear();
    }

    void drop_current_frame() {
        if (!current_access_unit_.empty() || assembling_fragment_ || !current_fragment_.empty()) {
            ++stats_.dropped_frames;
        }
        current_access_unit_.clear();
        current_fragment_.clear();
        assembling_fragment_ = false;
    }

    H264RtpDepacketizerConfig config_;
    H264RtpDepacketizerStats stats_;
    std::map<uint16_t, RtpPacket> reorder_buffer_;
    bool expected_sequence_initialized_ = false;
    uint16_t expected_sequence_number_ = 0;
    bool base_timestamp_initialized_ = false;
    uint32_t base_timestamp_ = 0;
    uint32_t current_frame_timestamp_ = 0;
    bool assembling_fragment_ = false;
    std::vector<uint8_t> current_access_unit_;
    std::vector<uint8_t> current_fragment_;
};

}  // namespace streambridge

