#include "native_video_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <android/log.h>

namespace streambridge::android {
namespace {

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(r);
}

streambridge::Result<void> invalid_frame(const char* message) {
    return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Config,
            streambridge::ErrorCode::InvalidArgument,
            message);
}

// Calculate letterbox rectangle: fit (src_w, src_h) into (dst_w, dst_h)
// preserving aspect ratio, centered. Returns offset and scaled size.
struct LetterBox {
    int x = 0, y = 0;
    int w = 0, h = 0;
};

LetterBox calc_letterbox(int src_w, int src_h, int dst_w, int dst_h) {
    LetterBox lb;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return lb;
    }

    double src_ratio = static_cast<double>(src_w) / src_h;
    double dst_ratio = static_cast<double>(dst_w) / dst_h;

    if (src_ratio > dst_ratio) {
        // Source wider -> fit by width, pillarbox (black bars top/bottom)
        lb.w = dst_w;
        lb.h = static_cast<int>(std::round(dst_w / src_ratio));
        lb.x = 0;
        lb.y = (dst_h - lb.h) / 2;
    } else {
        // Source taller -> fit by height, letterbox (black bars left/right)
        lb.w = static_cast<int>(std::round(dst_h * src_ratio));
        lb.h = dst_h;
        lb.x = (dst_w - lb.w) / 2;
        lb.y = 0;
    }

    // Clamp
    lb.w = std::max(1, std::min(lb.w, dst_w));
    lb.h = std::max(1, std::min(lb.h, dst_h));
    lb.x = std::max(0, std::min(lb.x, dst_w - lb.w));
    lb.y = std::max(0, std::min(lb.y, dst_h - lb.h));

    return lb;
}

// Bilinear-scaled pixel fetch from source RGBA image
inline uint32_t sample_bilinear(const uint8_t* src, int src_stride,
                                int src_w, int src_h,
                                float u, float v) {
    // Clamp to source bounds
    u = std::max(0.0f, std::min(u, static_cast<float>(src_w - 1)));
    v = std::max(0.0f, std::min(v, static_cast<float>(src_h - 1)));

    int x0 = static_cast<int>(u);
    int y0 = static_cast<int>(v);
    int x1 = std::min(x0 + 1, src_w - 1);
    int y1 = std::min(y0 + 1, src_h - 1);

    float fx = u - x0;
    float fy = v - y0;

    const uint8_t* p00 = src + y0 * src_stride + x0 * 4;
    const uint8_t* p10 = src + y0 * src_stride + x1 * 4;
    const uint8_t* p01 = src + y1 * src_stride + x0 * 4;
    const uint8_t* p11 = src + y1 * src_stride + x1 * 4;

    uint8_t r = static_cast<uint8_t>(
        (1 - fy) * ((1 - fx) * p00[0] + fx * p10[0]) +
        fy * ((1 - fx) * p01[0] + fx * p11[0]));
    uint8_t g = static_cast<uint8_t>(
        (1 - fy) * ((1 - fx) * p00[1] + fx * p10[1]) +
        fy * ((1 - fx) * p01[1] + fx * p11[1]));
    uint8_t b = static_cast<uint8_t>(
        (1 - fy) * ((1 - fx) * p00[2] + fx * p10[2]) +
        fy * ((1 - fx) * p01[2] + fx * p11[2]));

    return pack_rgba(r, g, b);
}

}  // namespace

NativeVideoRenderer::NativeVideoRenderer() = default;

NativeVideoRenderer::~NativeVideoRenderer() {
    clear_surface();
}

void NativeVideoRenderer::set_surface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (window_ == window) {
        return;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
    }
    window_ = window;
    last_buf_width_ = -1;
    last_buf_height_ = -1;
    if (window_ != nullptr) {
        ANativeWindow_acquire(window_);
    }
}

void NativeVideoRenderer::clear_surface() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    last_buf_width_ = -1;
    last_buf_height_ = -1;
}

streambridge::Result<void> NativeVideoRenderer::render(const streambridge::VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (window_ == nullptr) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceNotFound,
                "surface is not ready");
    }
    if (!frame.is_valid()) {
        return invalid_frame("video frame is invalid");
    }

    switch (frame.format) {
        case streambridge::PixelFormat::RGBA:
            return render_rgba(frame, false);
        case streambridge::PixelFormat::BGRA:
            return render_rgba(frame, true);
        case streambridge::PixelFormat::YUV420P:
            return render_yuv420p(frame);
        default:
            return streambridge::Result<void>::err(
                    streambridge::ErrorDomain::Codec,
                    streambridge::ErrorCode::CodecFormatUnsupported,
                    "unsupported video frame format");
    }
}

streambridge::Result<void> NativeVideoRenderer::render_rgba(
        const streambridge::VideoFrame& frame,
        bool source_is_bgra) {
    if (frame.num_planes < 1 || frame.planes[0].data == nullptr || frame.planes[0].stride <= 0) {
        return invalid_frame("RGBA frame plane is invalid");
    }

    // Let the buffer match the SurfaceView's natural size (set 0,0).
    // We handle aspect ratio via letterbox, not buffer geometry.
    if (last_buf_width_ < 0) {
        ANativeWindow_setBuffersGeometry(window_, 0, 0,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = 0;   // mark as set, don't re-call
        last_buf_height_ = 0;
        __android_log_print(ANDROID_LOG_INFO, "StreamBridgeRender",
                "buffer geometry set to native size");
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    const uint8_t* src = frame.planes[0].data;
    const int src_w = frame.width;
    const int src_h = frame.height;
    const int src_stride = frame.planes[0].stride;

    // Calculate letterbox: fit frame into buffer preserving aspect ratio
    const int buf_w = static_cast<int>(buffer.width);
    const int buf_h = static_cast<int>(buffer.height);
    auto lb = calc_letterbox(src_w, src_h, buf_w, buf_h);

    auto* dst = static_cast<uint32_t*>(buffer.bits);

    // Fill entire buffer with black first
    const uint32_t black = pack_rgba(0, 0, 0);
    for (int y = 0; y < buf_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        std::fill(dst_row, dst_row + buf_w, black);
    }

    // Scale frame into the letterbox area (bilinear interpolation)
    const float scale_x = static_cast<float>(src_w) / lb.w;
    const float scale_y = static_cast<float>(src_h) / lb.h;

    for (int dy = 0; dy < lb.h; ++dy) {
        uint32_t* dst_row = dst + (lb.y + dy) * buffer.stride + lb.x;
        const float src_v = (dy + 0.5f) * scale_y - 0.5f;

        // For downscaling (phone <= 720p), use fast nearest-neighbor
        if (scale_x >= 1.0f && scale_y >= 1.0f) {
            const int sy = static_cast<int>(src_v + 0.5f);
            if (sy < 0 || sy >= src_h) continue;
            const uint8_t* src_row = src + sy * src_stride;

            for (int dx = 0; dx < lb.w; ++dx) {
                const float src_u = (dx + 0.5f) * scale_x - 0.5f;
                const int sx = static_cast<int>(src_u + 0.5f);
                if (sx < 0 || sx >= src_w) continue;
                const uint8_t* px = src_row + sx * 4;
                const uint8_t r = source_is_bgra ? px[2] : px[0];
                const uint8_t g = px[1];
                const uint8_t b2 = source_is_bgra ? px[0] : px[2];
                dst_row[dx] = pack_rgba(r, g, b2);
            }
        } else {
            // Upscaling: bilinear
            for (int dx = 0; dx < lb.w; ++dx) {
                const float src_u = (dx + 0.5f) * scale_x - 0.5f;
                dst_row[dx] = sample_bilinear(src, src_stride, src_w, src_h,
                                              src_u, src_v);
                if (source_is_bgra) {
                    // Swap R/B from bilinear sample
                    uint32_t px = dst_row[dx];
                    uint8_t r = (px >> 0) & 0xFF;
                    uint8_t g = (px >> 8) & 0xFF;
                    uint8_t b2 = (px >> 16) & 0xFF;
                    uint8_t a = (px >> 24) & 0xFF;
                    dst_row[dx] = pack_rgba(b2, g, r, a);
                }
            }
        }
    }

    // Diagnostic: first frame only
    static int frame_count = 0;
    if (frame_count++ == 0) {
        __android_log_print(ANDROID_LOG_INFO, "StreamBridgeRender",
                "first render: src=%dx%d buf=%dx%d lb=%d,%d %dx%d",
                src_w, src_h, buf_w, buf_h,
                lb.x, lb.y, lb.w, lb.h);
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

streambridge::Result<void> NativeVideoRenderer::render_yuv420p(
        const streambridge::VideoFrame& frame) {
    if (frame.num_planes < 3 ||
            frame.planes[0].data == nullptr ||
            frame.planes[1].data == nullptr ||
            frame.planes[2].data == nullptr) {
        return invalid_frame("YUV420P frame planes are invalid");
    }

    if (last_buf_width_ < 0) {
        ANativeWindow_setBuffersGeometry(window_, 0, 0,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = 0;
        last_buf_height_ = 0;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    const int src_w = frame.width;
    const int src_h = frame.height;
    const int buf_w = static_cast<int>(buffer.width);
    const int buf_h = static_cast<int>(buffer.height);
    auto lb = calc_letterbox(src_w, src_h, buf_w, buf_h);

    auto* dst = static_cast<uint32_t*>(buffer.bits);

    // Fill black background
    const uint32_t black = pack_rgba(0, 0, 0);
    for (int y = 0; y < buf_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        std::fill(dst_row, dst_row + buf_w, black);
    }

    // YUV->RGB in the letterbox area (nearest-neighbor for speed)
    const float scale_x = static_cast<float>(src_w) / lb.w;
    const float scale_y = static_cast<float>(src_h) / lb.h;

    for (int dy = 0; dy < lb.h; ++dy) {
        uint32_t* dst_row = dst + (lb.y + dy) * buffer.stride + lb.x;
        const int sy = std::clamp(
            static_cast<int>((dy + 0.5f) * scale_y - 0.5f), 0, src_h - 1);

        const uint8_t* y_row = frame.planes[0].data + sy * frame.planes[0].stride;
        const uint8_t* u_row = frame.planes[1].data + (sy / 2) * frame.planes[1].stride;
        const uint8_t* v_row = frame.planes[2].data + (sy / 2) * frame.planes[2].stride;

        for (int dx = 0; dx < lb.w; ++dx) {
            const int sx = std::clamp(
                static_cast<int>((dx + 0.5f) * scale_x - 0.5f), 0, src_w - 1);

            const int yy = static_cast<int>(y_row[sx]);
            const int uu = static_cast<int>(u_row[sx / 2]) - 128;
            const int vv = static_cast<int>(v_row[sx / 2]) - 128;

            auto clamp_u8 = [](int v) -> uint8_t {
                return static_cast<uint8_t>(std::max(0, std::min(255, v)));
            };

            const uint8_t r = clamp_u8(yy + ((359 * vv) >> 8));
            const uint8_t g = clamp_u8(yy - ((88 * uu + 183 * vv) >> 8));
            const uint8_t b = clamp_u8(yy + ((454 * uu) >> 8));
            dst_row[dx] = pack_rgba(r, g, b);
        }
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

}  // namespace streambridge::android
