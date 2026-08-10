// Linux-specific FFmpeg utilities
// Common FFmpeg helpers are in common/include/streambridge/ffmpeg_utils.h
// and common/src/ffmpeg_utils.cpp
#include "ffmpeg_utils.h"

namespace streambridge {

// ============================================================
// YUV420P layout — used by V4L2/ALSA capture
// ============================================================

Yuv420PLayout compute_yuv420p_layout(int width, int height) {
    Yuv420PLayout lo;
    lo.y_stride = width;
    lo.uv_stride = (width + 1) / 2;
    int uv_h = (height + 1) / 2;
    lo.y_size = static_cast<size_t>(lo.y_stride) * height;
    lo.u_size = static_cast<size_t>(lo.uv_stride) * uv_h;
    lo.v_size = static_cast<size_t>(lo.uv_stride) * uv_h;
    lo.total_size = lo.y_size + lo.u_size + lo.v_size;
    return lo;
}

}  // namespace streambridge
