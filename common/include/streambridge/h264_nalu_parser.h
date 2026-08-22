#pragma once
// H.264 NALU parsing helpers. Platform-independent and zero-copy.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "streambridge/media_types.h"

namespace streambridge {

struct H264NaluView {
    const uint8_t* data = nullptr;
    size_t size = 0;

    bool empty() const { return data == nullptr || size == 0; }
    uint8_t nal_type() const { return empty() ? 0 : static_cast<uint8_t>(data[0] & 0x1F); }
    bool is_sps() const { return nal_type() == 7; }
    bool is_pps() const { return nal_type() == 8; }
    bool is_idr() const { return nal_type() == 5; }
};

struct H264NaluParseResult {
    bool ok = false;
    H264PacketFormat format = H264PacketFormat::Unknown;
    int avcc_length_size = 0;
    std::vector<H264NaluView> nalus;
    std::string error;
};

inline bool h264_has_annexb_start_code_at(const uint8_t* data, size_t size, size_t offset,
                                          size_t* start_code_size) {
    if (data == nullptr || offset + 3 > size) {
        return false;
    }
    if (data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
        if (start_code_size) *start_code_size = 3;
        return true;
    }
    if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1) {
        if (start_code_size) *start_code_size = 4;
        return true;
    }
    return false;
}

inline bool h264_has_annexb_start_code(const uint8_t* data, size_t size) {
    return h264_has_annexb_start_code_at(data, size, 0, nullptr);
}

inline uint32_t h264_read_be_length(const uint8_t* data, int length_size) {
    uint32_t value = 0;
    for (int i = 0; i < length_size; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

inline bool h264_detect_avcc_length_size(const uint8_t* data, size_t size,
                                         int* out_length_size) {
    if (out_length_size) *out_length_size = 0;
    if (data == nullptr || size < 2) {
        return false;
    }

    for (int length_size : {4, 2, 1}) {
        if (size < static_cast<size_t>(length_size + 1)) {
            continue;
        }
        size_t offset = 0;
        int nal_count = 0;
        bool ok = true;
        while (offset + static_cast<size_t>(length_size) <= size) {
            const uint32_t nal_size = h264_read_be_length(data + offset, length_size);
            offset += static_cast<size_t>(length_size);
            if (nal_size == 0 || nal_size > size - offset) {
                ok = false;
                break;
            }
            offset += nal_size;
            ++nal_count;
        }
        if (ok && nal_count > 0 && offset == size) {
            if (out_length_size) *out_length_size = length_size;
            return true;
        }
    }
    return false;
}

inline H264PacketFormat h264_detect_packet_format(const uint8_t* data, size_t size,
                                                  int* out_avcc_length_size = nullptr) {
    if (out_avcc_length_size) *out_avcc_length_size = 0;
    if (h264_has_annexb_start_code(data, size)) {
        return H264PacketFormat::AnnexB;
    }
    int length_size = 0;
    if (h264_detect_avcc_length_size(data, size, &length_size)) {
        if (out_avcc_length_size) *out_avcc_length_size = length_size;
        return H264PacketFormat::AvccLengthPrefixed;
    }
    return H264PacketFormat::Unknown;
}

inline H264NaluParseResult h264_parse_annexb_nalus(const uint8_t* data, size_t size) {
    H264NaluParseResult result;
    result.format = H264PacketFormat::AnnexB;
    if (data == nullptr || size == 0) {
        result.error = "empty Annex-B buffer";
        return result;
    }

    size_t offset = 0;
    while (offset < size) {
        size_t start_code_size = 0;
        size_t start_code_pos = size;
        for (size_t i = offset; i + 3 <= size; ++i) {
            if (h264_has_annexb_start_code_at(data, size, i, &start_code_size)) {
                start_code_pos = i;
                break;
            }
        }
        if (start_code_pos == size) {
            break;
        }

        const size_t nalu_start = start_code_pos + start_code_size;
        size_t next_start_code_pos = size;
        for (size_t i = nalu_start; i + 3 <= size; ++i) {
            if (h264_has_annexb_start_code_at(data, size, i, nullptr)) {
                next_start_code_pos = i;
                break;
            }
        }

        if (next_start_code_pos > nalu_start) {
            result.nalus.push_back({data + nalu_start, next_start_code_pos - nalu_start});
        }
        offset = next_start_code_pos;
    }

    result.ok = !result.nalus.empty();
    if (!result.ok) {
        result.error = "Annex-B buffer contains no NAL units";
    }
    return result;
}

inline H264NaluParseResult h264_parse_avcc_nalus(const uint8_t* data, size_t size,
                                                 int length_size) {
    H264NaluParseResult result;
    result.format = H264PacketFormat::AvccLengthPrefixed;
    result.avcc_length_size = length_size;
    if (data == nullptr || size == 0) {
        result.error = "empty AVCC buffer";
        return result;
    }
    if (length_size != 1 && length_size != 2 && length_size != 4) {
        result.error = "invalid AVCC length size";
        return result;
    }

    size_t offset = 0;
    while (offset + static_cast<size_t>(length_size) <= size) {
        const uint32_t nal_size = h264_read_be_length(data + offset, length_size);
        offset += static_cast<size_t>(length_size);
        if (nal_size == 0 || nal_size > size - offset) {
            result.error = "malformed AVCC NAL length";
            return result;
        }
        result.nalus.push_back({data + offset, nal_size});
        offset += nal_size;
    }

    if (offset != size || result.nalus.empty()) {
        result.error = "AVCC buffer contains trailing bytes or no NAL units";
        return result;
    }
    result.ok = true;
    return result;
}

inline H264NaluParseResult h264_parse_nalus(const uint8_t* data, size_t size,
                                            H264PacketFormat format,
                                            int avcc_length_size = 0) {
    if (format == H264PacketFormat::AnnexB) {
        return h264_parse_annexb_nalus(data, size);
    }
    if (format == H264PacketFormat::AvccLengthPrefixed) {
        int length_size = avcc_length_size;
        if (length_size == 0 && !h264_detect_avcc_length_size(data, size, &length_size)) {
            H264NaluParseResult result;
            result.format = H264PacketFormat::AvccLengthPrefixed;
            result.error = "unable to detect AVCC length size";
            return result;
        }
        return h264_parse_avcc_nalus(data, size, length_size);
    }
    H264NaluParseResult result;
    result.error = "unknown H.264 packet format";
    return result;
}

}  // namespace streambridge