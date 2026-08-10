#include "mediacodec_csd.h"

#include <cstring>

namespace streambridge::android::mediacodec {

namespace {

constexpr uint8_t kAnnexBStartCode[] = {0x00, 0x00, 0x00, 0x01};

void append_start_code(std::vector<uint8_t>& buf) {
    buf.insert(buf.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
}

void append_nal(std::vector<uint8_t>& buf, const streambridge::ffmpeg::NalUnit& nal) {
    append_start_code(buf);
    buf.insert(buf.end(), nal.data.begin(), nal.data.end());
}

}  // namespace

MediaCodecCsd build_mediacodec_csd(const streambridge::ffmpeg::CodecConfig& config) {
    MediaCodecCsd csd;

    if (config.codec_id == AV_CODEC_ID_H264) {
        // csd-0: 00 00 00 01 + SPS
        if (!config.sps_list.empty()) {
            append_nal(csd.csd_0, config.sps_list[0]);
        }
        // Append extra SPS if present
        for (size_t i = 1; i < config.sps_list.size(); ++i) {
            append_nal(csd.csd_0, config.sps_list[i]);
        }

        // csd-1: 00 00 00 01 + PPS
        if (!config.pps_list.empty()) {
            append_nal(csd.csd_1, config.pps_list[0]);
        }
        for (size_t i = 1; i < config.pps_list.size(); ++i) {
            append_nal(csd.csd_1, config.pps_list[i]);
        }
    } else if (config.codec_id == AV_CODEC_ID_H265) {
        // csd-0: 00 00 00 01 + VPS + 00 00 00 01 + SPS + 00 00 00 01 + PPS
        for (auto& vps : config.vps_list) {
            append_nal(csd.csd_0, vps);
        }
        for (auto& sps : config.sps_list) {
            append_nal(csd.csd_0, sps);
        }
        for (auto& pps : config.pps_list) {
            append_nal(csd.csd_0, pps);
        }
        // csd-1 empty for H.265
    }

    return csd;
}

}  // namespace streambridge::android::mediacodec
