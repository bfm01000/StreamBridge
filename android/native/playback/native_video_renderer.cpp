#include "native_video_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "streambridge/logging.h"
#include "streambridge/video_utils.h"

namespace streambridge::android {
namespace {

using streambridge::pack_rgba;
using streambridge::calc_letterbox;
using streambridge::sample_bilinear;
using streambridge::yuv_to_rgb;
using streambridge::clamp_u8;

streambridge::Result<void> invalid_frame(const char* message) {
    return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Config,
            streambridge::ErrorCode::InvalidArgument,
            message);
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
        SB_LOG_I("StreamBridgeRender", "buffer geometry set to native size");
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
        SB_LOG_I("StreamBridgeRender",
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
