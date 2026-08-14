#pragma once
// Linux platform FFmpeg extensions
// Includes the common ffmpeg_utils.h and adds Linux-specific helpers

#include "streambridge/ffmpeg_utils.h"

namespace streambridge {

// ============================================================
// Linux-specific helpers
// ============================================================

inline AVFormatContextPtr alloc_input_format_context() {
    AVFormatContext* ctx = avformat_alloc_context();
    return AVFormatContextPtr(ctx);
}

// ============================================================
// YUV420P layout calculation
// ============================================================

struct Yuv420PLayout {
    int y_stride;
    int uv_stride;
    size_t y_size;
    size_t u_size;
    size_t v_size;
    size_t total_size;
};

inline Yuv420PLayout compute_yuv420p_layout(int width, int height) {
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

// AVRational → Rational
inline Rational from_avrational(AVRational r) { return {r.num, r.den}; }

}  // namespace streambridge
