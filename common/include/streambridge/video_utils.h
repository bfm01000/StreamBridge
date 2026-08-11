#pragma once
// Platform-independent pixel & video utilities
// — letterbox / aspect-fit calculation
// — RGBA pixel packing
// — YUV420P → RGB conversion
// — bilinear sampling

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace streambridge {

// ============================================================
// RGBA pixel packing
// ============================================================

inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(r);
}

inline uint32_t pack_bgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

// ============================================================
// Letterbox / aspect-fit rectangle
// ============================================================

// 信箱模式矩形：源图像等比缩放居中后在目标区域的坐标与尺寸
struct LetterBox {
    int x = 0, y = 0;
    int w = 0, h = 0;
};

// Fit (src_w, src_h) into (dst_w, dst_h) preserving aspect ratio, centered
inline LetterBox calc_letterbox(int src_w, int src_h, int dst_w, int dst_h) {
    LetterBox lb;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return lb;

    double src_ratio = static_cast<double>(src_w) / src_h;
    double dst_ratio = static_cast<double>(dst_w) / dst_h;

    if (src_ratio > dst_ratio) {
        lb.w = dst_w;
        lb.h = static_cast<int>(std::round(dst_w / src_ratio));
        lb.x = 0;
        lb.y = (dst_h - lb.h) / 2;
    } else {
        lb.w = static_cast<int>(std::round(dst_h * src_ratio));
        lb.h = dst_h;
        lb.x = (dst_w - lb.w) / 2;
        lb.y = 0;
    }

    lb.w = std::max(1, std::min(lb.w, dst_w));
    lb.h = std::max(1, std::min(lb.h, dst_h));
    lb.x = std::max(0, std::min(lb.x, dst_w - lb.w));
    lb.y = std::max(0, std::min(lb.y, dst_h - lb.h));
    return lb;
}

// ============================================================
// Bilinear sample from RGBA image
// ============================================================

inline uint32_t sample_bilinear(const uint8_t* src, int src_stride,
                                 int src_w, int src_h,
                                 float u, float v) {
    u = std::max(0.0f, std::min(u, static_cast<float>(src_w - 1)));
    v = std::max(0.0f, std::min(v, static_cast<float>(src_h - 1)));

    int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
    int x1 = std::min(x0 + 1, src_w - 1);
    int y1 = std::min(y0 + 1, src_h - 1);

    float fx = u - x0, fy = v - y0;

    const uint8_t* p00 = src + y0 * src_stride + x0 * 4;
    const uint8_t* p10 = src + y0 * src_stride + x1 * 4;
    const uint8_t* p01 = src + y1 * src_stride + x0 * 4;
    const uint8_t* p11 = src + y1 * src_stride + x1 * 4;

    uint8_t r = static_cast<uint8_t>(
        (1 - fy) * ((1 - fx) * p00[0] + fx * p10[0]) +
         fy      * ((1 - fx) * p01[0] + fx * p11[0]));
    uint8_t g = static_cast<uint8_t>(
        (1 - fy) * ((1 - fx) * p00[1] + fx * p10[1]) +
         fy      * ((1 - fx) * p01[1] + fx * p11[1]));
    uint8_t b = static_cast<uint8_t>(
        (1 - fy) * ((1 - fx) * p00[2] + fx * p10[2]) +
         fy      * ((1 - fx) * p01[2] + fx * p11[2]));

    return pack_rgba(r, g, b);
}

// ============================================================
// YUV420P → RGBA conversion (single pixel, ITU-R BT.601)
// ============================================================

inline uint8_t clamp_u8(int v) {
    return static_cast<uint8_t>(std::max(0, std::min(255, v)));
}

inline void yuv_to_rgb(int yy, int uu, int vv,
                        uint8_t& r, uint8_t& g, uint8_t& b) {
    int c = yy;
    int d = uu - 128;
    int e = vv - 128;
    r = clamp_u8(c + ((359 * e) >> 8));
    g = clamp_u8(c - (( 88 * d + 183 * e) >> 8));
    b = clamp_u8(c + ((454 * d) >> 8));
}

inline uint32_t yuv420p_to_rgba(const uint8_t* y_plane, const uint8_t* u_plane,
                                  const uint8_t* v_plane,
                                  int y_stride, int uv_stride,
                                  int x, int y) {
    int yy = static_cast<int>(y_plane[y * y_stride + x]);
    int uu = static_cast<int>(u_plane[(y / 2) * uv_stride + x / 2]);
    int vv = static_cast<int>(v_plane[(y / 2) * uv_stride + x / 2]);
    uint8_t r, g, b;
    yuv_to_rgb(yy, uu, vv, r, g, b);
    return pack_rgba(r, g, b);
}

}  // namespace streambridge
