#include "video_path_config.h"

namespace streambridge::android {

VideoDecodePath video_decode_path_from_int(int value) {
    switch (value) {
        case 1:
            return VideoDecodePath::MediaCodecAhbGpu;
        case 2:
            return VideoDecodePath::MediaCodecSurface;
        case 3:
            return VideoDecodePath::FFmpegSoftware;
        case 0:
        default:
            return VideoDecodePath::Auto;
    }
}

const char* video_decode_path_name(VideoDecodePath path) {
    switch (path) {
        case VideoDecodePath::Auto:
            return "AUTO";
        case VideoDecodePath::MediaCodecAhbGpu:
            return "MEDIACODEC_AHB_GPU";
        case VideoDecodePath::MediaCodecSurface:
            return "MEDIACODEC_SURFACE";
        case VideoDecodePath::FFmpegSoftware:
            return "FFMPEG_SOFTWARE";
        default:
            return "UNKNOWN";
    }
}

}  // namespace streambridge::android
