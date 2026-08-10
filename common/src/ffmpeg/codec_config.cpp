#include "codec_config.h"

#include <algorithm>
#include <cstring>

namespace streambridge::ffmpeg {

// ============================================================
// Internal helpers
// ============================================================

namespace {

// Read big-endian uint16 from byte buffer
inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

constexpr uint8_t kStartCode3[] = {0x00, 0x00, 0x01};
constexpr uint8_t kStartCode4[] = {0x00, 0x00, 0x00, 0x01};

// Find next start code in data[offset..size), returns position or -1
// Also returns start_code_len (3 or 4)
int find_start_code(const uint8_t* data, size_t size, size_t offset,
                    int& start_code_len) {
    for (size_t i = offset; i + 3 <= size; ++i) {
        if (memcmp(data + i, kStartCode3, 3) == 0) {
            start_code_len = 3;
            return static_cast<int>(i);
        }
        if (i + 4 <= size && memcmp(data + i, kStartCode4, 4) == 0) {
            start_code_len = 4;
            return static_cast<int>(i);
        }
    }
    return -1;
}

// H.264 NAL unit type from first byte
inline int h264_nal_type(uint8_t b) { return b & 0x1F; }

// H.265 NAL unit type from header (2 bytes: (hdr[0] >> 1) & 0x3F)
inline int h265_nal_type(const uint8_t* hdr) {
    return (hdr[0] >> 1) & 0x3F;
}

// H.264 NAL types
constexpr int kNalSps = 7;
constexpr int kNalPps = 8;

// H.265 NAL types
constexpr int kNalVps = 32;
constexpr int kNalSpsHevc = 33;
constexpr int kNalPpsHevc = 34;

}  // namespace

// ============================================================
// Format detection
// ============================================================

BitstreamFormat detect_bitstream_format(AVCodecID codec_id,
                                         const uint8_t* data, size_t size) {
    if (data == nullptr || size < 4) return BitstreamFormat::Unknown;
    if (codec_id == AV_CODEC_ID_H264) {
        // avcC: first byte is configurationVersion (=1)
        if (data[0] == 1) return BitstreamFormat::Avcc;
        // AnnexB: starts with start code
        if (size >= 3 && memcmp(data, kStartCode3, 3) == 0) return BitstreamFormat::AnnexB;
        if (size >= 4 && memcmp(data, kStartCode4, 4) == 0) return BitstreamFormat::AnnexB;
    } else if (codec_id == AV_CODEC_ID_H265) {
        // hvcC: first byte is configurationVersion (=1), second byte has profile info
        if (data[0] == 1) return BitstreamFormat::Hvcc;
        if (size >= 3 && memcmp(data, kStartCode3, 3) == 0) return BitstreamFormat::AnnexB;
        if (size >= 4 && memcmp(data, kStartCode4, 4) == 0) return BitstreamFormat::AnnexB;
    }
    return BitstreamFormat::Unknown;
}

// ============================================================
// Annex-B parser (H.264 and H.265)
// ============================================================

static Result<CodecConfig> parse_annexb(AVCodecID codec_id,
                                         const uint8_t* data, size_t size) {
    CodecConfig config;
    config.codec_id = codec_id;
    config.format = BitstreamFormat::AnnexB;

    size_t offset = 0;
    while (offset < size) {
        int sc_len = 0;
        int sc_pos = find_start_code(data, size, offset, sc_len);
        if (sc_pos < 0) break;

        size_t nal_start = sc_pos + sc_len;
        // Find next start code to determine NAL end
        int next_sc_len = 0;
        int next_sc = find_start_code(data, size, nal_start, next_sc_len);

        size_t nal_end = (next_sc >= 0) ? static_cast<size_t>(next_sc) : size;
        if (nal_end <= nal_start) break;

        const uint8_t* nal_data = data + nal_start;
        size_t nal_size = nal_end - nal_start;

        if (codec_id == AV_CODEC_ID_H264) {
            int type = h264_nal_type(nal_data[0]);
            if (type == kNalSps) {
                config.sps_list.push_back({std::vector<uint8_t>(nal_data, nal_data + nal_size)});
            } else if (type == kNalPps) {
                config.pps_list.push_back({std::vector<uint8_t>(nal_data, nal_data + nal_size)});
            }
        } else if (codec_id == AV_CODEC_ID_H265) {
            int type = h265_nal_type(nal_data);
            if (type == kNalVps) {
                config.vps_list.push_back({std::vector<uint8_t>(nal_data, nal_data + nal_size)});
            } else if (type == kNalSpsHevc) {
                config.sps_list.push_back({std::vector<uint8_t>(nal_data, nal_data + nal_size)});
            } else if (type == kNalPpsHevc) {
                config.pps_list.push_back({std::vector<uint8_t>(nal_data, nal_data + nal_size)});
            }
        }

        offset = nal_end;
    }

    return Result<CodecConfig>::ok(std::move(config));
}

// ============================================================
// avcC parser
// ============================================================

static Result<CodecConfig> parse_avcc(const uint8_t* data, size_t size) {
    CodecConfig config;
    config.codec_id = AV_CODEC_ID_H264;
    config.format = BitstreamFormat::Avcc;

    // Minimum avcC size: 7 bytes header
    if (size < 7) {
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedAvcc,
                                         "avcC too short");
    }

    if (data[0] != 1) {
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedAvcc,
                                         "avcC: invalid configurationVersion");
    }

    config.nal_length_size = 1 + (data[4] & 0x03);  // lengthSizeMinusOne + 1

    size_t offset = 5;
    int num_sps = data[offset++] & 0x1F;
    if (num_sps == 0) {
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MissingSps,
                                         "avcC: no SPS");
    }

    for (int i = 0; i < num_sps; ++i) {
        if (offset + 2 > size) {
            return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedAvcc,
                                             "avcC: SPS length overflow");
        }
        uint16_t sps_len = read_u16(data + offset);
        offset += 2;
        if (offset + sps_len > size) {
            return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedAvcc,
                                             "avcC: SPS data overflow");
        }
        NalUnit nal;
        nal.data.assign(data + offset, data + offset + sps_len);
        config.sps_list.push_back(std::move(nal));
        offset += sps_len;
    }

    if (offset >= size) {
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MissingPps,
                                         "avcC: no PPS");
    }
    int num_pps = data[offset++];
    if (num_pps == 0) {
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MissingPps,
                                         "avcC: no PPS entries");
    }

    for (int i = 0; i < num_pps; ++i) {
        if (offset + 2 > size) {
            return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedAvcc,
                                             "avcC: PPS length overflow");
        }
        uint16_t pps_len = read_u16(data + offset);
        offset += 2;
        if (offset + pps_len > size) {
            return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedAvcc,
                                             "avcC: PPS data overflow");
        }
        NalUnit nal;
        nal.data.assign(data + offset, data + offset + pps_len);
        config.pps_list.push_back(std::move(nal));
        offset += pps_len;
    }

    return Result<CodecConfig>::ok(std::move(config));
}

// ============================================================
// hvcC parser
// ============================================================

static Result<CodecConfig> parse_hvcc(const uint8_t* data, size_t size) {
    CodecConfig config;
    config.codec_id = AV_CODEC_ID_H265;
    config.format = BitstreamFormat::Hvcc;

    if (size < 23) {  // minimum hvcC header
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedHvcc,
                                         "hvcC too short");
    }

    if (data[0] != 1) {
        return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedHvcc,
                                         "hvcC: invalid configurationVersion");
    }

    // byte 21: lengthSizeMinusOne (low 2 bits) + numArrays (high 6 bits is reserved/flags)
    config.nal_length_size = 1 + (data[21] & 0x03);

    // numArrays at byte 22
    int num_arrays = static_cast<int>(data[22]);
    size_t offset = 23;

    for (int a = 0; a < num_arrays; ++a) {
        if (offset + 3 > size) {
            return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedHvcc,
                                             "hvcC: array header overflow");
        }
        // byte 0: bit(1) array_completeness + reserved(1) + NAL_unit_type(6)
        int nal_type = data[offset] & 0x3F;
        offset += 1;
        int num_nalus = read_u16(data + offset);
        offset += 2;

        for (int n = 0; n < num_nalus; ++n) {
            if (offset + 2 > size) {
                return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedHvcc,
                                                 "hvcC: NALU length overflow");
            }
            uint16_t nal_len = read_u16(data + offset);
            offset += 2;
            if (offset + nal_len > size) {
                return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::MalformedHvcc,
                                                 "hvcC: NALU data overflow");
            }

            NalUnit nal;
            nal.data.assign(data + offset, data + offset + nal_len);
            offset += nal_len;

            if (nal_type == kNalVps) {
                config.vps_list.push_back(std::move(nal));
            } else if (nal_type == kNalSpsHevc) {
                config.sps_list.push_back(std::move(nal));
            } else if (nal_type == kNalPpsHevc) {
                config.pps_list.push_back(std::move(nal));
            }
        }
    }

    return Result<CodecConfig>::ok(std::move(config));
}

// ============================================================
// Public API
// ============================================================

Result<CodecConfig> parse_codec_config(AVCodecID codec_id,
                                        const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        CodecConfig empty;
        empty.codec_id = codec_id;
        return Result<CodecConfig>::ok(std::move(empty));
    }

    auto format = detect_bitstream_format(codec_id, data, size);

    switch (format) {
        case BitstreamFormat::Avcc:
            return parse_avcc(data, size);
        case BitstreamFormat::Hvcc:
            return parse_hvcc(data, size);
        case BitstreamFormat::AnnexB:
            return parse_annexb(codec_id, data, size);
        default:
            return Result<CodecConfig>::err(ErrorDomain::Codec, ErrorCode::InvalidCodecConfig,
                                             "unknown extradata format");
    }
}

Result<CodecConfig> parse_codec_config_from_packet(AVCodecID codec_id,
                                                    const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        CodecConfig empty;
        empty.codec_id = codec_id;
        return Result<CodecConfig>::ok(std::move(empty));
    }
    return parse_annexb(codec_id, data, size);
}

void merge_codec_config(CodecConfig& accumulated, const CodecConfig& packet_config) {
    accumulated.codec_id = packet_config.codec_id;
    if (accumulated.format == BitstreamFormat::Unknown) {
        accumulated.format = packet_config.format;
    }
    for (auto& sps : packet_config.sps_list) {
        accumulated.sps_list.push_back(sps);
    }
    for (auto& pps : packet_config.pps_list) {
        accumulated.pps_list.push_back(pps);
    }
    for (auto& vps : packet_config.vps_list) {
        accumulated.vps_list.push_back(vps);
    }
}

}  // namespace streambridge::ffmpeg
