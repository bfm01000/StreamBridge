#pragma once
// Build MediaCodec CSD buffers from platform-independent CodecConfig
// Converts parameter sets (SPS/PPS/VPS) to Annex-B start-code format for csd-0/csd-1

#include <cstdint>
#include <vector>

#include "ffmpeg/codec_config.h"

namespace streambridge::android::mediacodec {

struct MediaCodecCsd {
    std::vector<uint8_t> csd_0;
    std::vector<uint8_t> csd_1;  // PPS for H.264; empty for H.265
};

// Build MediaCodec codec-specific-data from parsed CodecConfig.
// csd-0 = 00 00 00 01 + SPS (H.264) or 00 00 00 01 + VPS + 00 00 00 01 + SPS + 00 00 00 01 + PPS (H.265)
// csd-1 = 00 00 00 01 + PPS (H.264 only)
MediaCodecCsd build_mediacodec_csd(const streambridge::ffmpeg::CodecConfig& config);

}  // namespace streambridge::android::mediacodec
