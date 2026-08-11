#include "native_video_renderer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "streambridge/logging.h"
#include "streambridge/video_utils.h"

namespace streambridge::android {
namespace {

using streambridge::pack_rgba;
using streambridge::yuv_to_rgb;

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

    // Keep the native buffer in source-frame geometry. This avoids an extra
    // software scaling pass and, more importantly, avoids device-dependent
    // Surface buffer stride/crop behavior when geometry is reset to 0x0.
    if (last_buf_width_ != frame.width || last_buf_height_ != frame.height) {
        ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = frame.width;
        last_buf_height_ = frame.height;
        SB_LOG_I("StreamBridgeRender", "buffer geometry set to frame size %dx%d",
                 frame.width, frame.height);
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

    const int buf_w = static_cast<int>(buffer.width);
    const int buf_h = static_cast<int>(buffer.height);

    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int copy_w = std::min(src_w, buf_w);
    const int copy_h = std::min(src_h, buf_h);

    for (int y = 0; y < copy_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* src_row = src + y * src_stride;
        if (!source_is_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * 4);
            continue;
        }
        for (int x = 0; x < copy_w; ++x) {
            const uint8_t* px = src_row + x * 4;
            dst_row[x] = pack_rgba(px[2], px[1], px[0], px[3]);
        }
    }

    // Diagnostic: first frame only
    static int frame_count = 0;
    if (frame_count++ == 0) {
        SB_LOG_I("StreamBridgeRender",
                "first render: src=%dx%d(stride=%d) buf=%dx%d(stride=%d) lb=%d,%d %dx%d scale=%.3f,%.3f",
                src_w, src_h, src_stride, buf_w, buf_h, (int)buffer.stride,
                0, 0, copy_w, copy_h, 1.0, 1.0);
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

    if (last_buf_width_ != frame.width || last_buf_height_ != frame.height) {
        ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = frame.width;
        last_buf_height_ = frame.height;
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
    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int copy_w = std::min(src_w, buf_w);
    const int copy_h = std::min(src_h, buf_h);

    for (int y = 0; y < copy_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* y_row = frame.planes[0].data + y * frame.planes[0].stride;
        const uint8_t* u_row = frame.planes[1].data + (y / 2) * frame.planes[1].stride;
        const uint8_t* v_row = frame.planes[2].data + (y / 2) * frame.planes[2].stride;

        for (int x = 0; x < copy_w; ++x) {
            uint8_t r, g, b;
            yuv_to_rgb(static_cast<int>(y_row[x]),
                       static_cast<int>(u_row[x / 2]),
                       static_cast<int>(v_row[x / 2]), r, g, b);
            dst_row[x] = pack_rgba(r, g, b);
        }
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

}  // namespace streambridge::android
