#pragma once
// Codec config parser — extracts SPS/PPS/VPS from extradata or bare Annex-B bitstream
// Platform-independent: lives in common, serves both Android MediaCodec and Linux VAAPI

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "streambridge/media_errors.h"

namespace streambridge::ffmpeg {

// ============================================================
// Bitstream format
// ============================================================

enum class BitstreamFormat {
    Unknown,
    AnnexB,   // 00 00 00 01 / 00 00 01 start-code delimited
    Avcc,     // AVCDecoderConfigurationRecord (FLV/MP4 H.264 extradata)
    Hvcc,     // HEVCDecoderConfigurationRecord (FLV/MP4 H.265 extradata)
};

inline const char* bitstream_format_name(BitstreamFormat f) {
    switch (f) {
        case BitstreamFormat::AnnexB: return "AnnexB";
        case BitstreamFormat::Avcc:   return "avcC";
        case BitstreamFormat::Hvcc:   return "hvcC";
        default: return "unknown";
    }
}

// ============================================================
// NAL unit (raw data, without start-code or length prefix)
// ============================================================

struct NalUnit {
    std::vector<uint8_t> data;
};

// ============================================================
// CodecConfig — parsed parameter sets
// ============================================================

struct CodecConfig {
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    BitstreamFormat format = BitstreamFormat::Unknown;
    int nal_length_size = 4;  // avcC/hvcC NALU length field size (1/2/4 bytes)

    // H.264
    std::vector<NalUnit> sps_list;
    std::vector<NalUnit> pps_list;

    // H.265
    std::vector<NalUnit> vps_list;

    // Returns true if enough parameter sets exist to configure a decoder
    bool is_complete() const {
        if (codec_id == AV_CODEC_ID_H264) {
            return !sps_list.empty() && !pps_list.empty();
        }
        if (codec_id == AV_CODEC_ID_H265) {
            return !vps_list.empty() && !sps_list.empty() && !pps_list.empty();
        }
        return false;
    }
};

// ============================================================
// Parser API
// ============================================================

// Parse extradata buffer into parameter sets.
// Handles avcC, hvcC, and Annex-B formats.
Result<CodecConfig> parse_codec_config(AVCodecID codec_id,
                                       const uint8_t* data, size_t size);

// Try to extract parameter sets from a raw Annex-B packet.
// Returns config with format=AnnexB and extracted SPS/PPS/VPS.
// If no parameter sets found, returns a config with is_complete()==false (not an error).
// Caller should accumulate results across multiple packets until is_complete().
Result<CodecConfig> parse_codec_config_from_packet(AVCodecID codec_id,
                                                    const uint8_t* data, size_t size);

// Merge parameter sets from `packet_config` into `accumulated`.
// Used for bare Annex-B streams where SPS/PPS arrive in separate packets.
void merge_codec_config(CodecConfig& accumulated, const CodecConfig& packet_config);

// Detect bitstream format from extradata header bytes
BitstreamFormat detect_bitstream_format(AVCodecID codec_id,
                                         const uint8_t* data, size_t size);

}  // namespace streambridge::ffmpeg
