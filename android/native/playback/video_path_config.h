#pragma once

#include <string>

namespace streambridge::android {

enum class VideoDecodePath {
    Auto = 0,
    MediaCodecAhbGpu = 1,
    MediaCodecSurface = 2,
    FFmpegSoftware = 3,
};

VideoDecodePath video_decode_path_from_int(int value);
const char* video_decode_path_name(VideoDecodePath path);

}  // namespace streambridge::android
