#include "native_video_renderer.h"

#include <algorithm>
#include <cstdint>

namespace streambridge::android {
namespace {

uint8_t clamp_to_u8(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

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

}  // namespace

NativeVideoRenderer::NativeVideoRenderer() = default;

NativeVideoRenderer::~NativeVideoRenderer() {
    clear_surface();
}

void NativeVideoRenderer::set_surface(ANativeWindow* window) {
    if (window_ == window) {
        return;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
    }
    window_ = window;
    if (window_ != nullptr) {
        ANativeWindow_acquire(window_);
    }
}

void NativeVideoRenderer::clear_surface() {
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

streambridge::Result<void> NativeVideoRenderer::render(const streambridge::VideoFrame& frame) {
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

    ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height, WINDOW_FORMAT_RGBA_8888);

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const uint8_t* src = frame.planes[0].data;
    const int width = std::min(frame.width, buffer.width);
    const int height = std::min(frame.height, buffer.height);

    for (int y = 0; y < height; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* src_row = src + y * frame.planes[0].stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = src_row + x * 4;
            const uint8_t r = source_is_bgra ? px[2] : px[0];
            const uint8_t g = px[1];
            const uint8_t b = source_is_bgra ? px[0] : px[2];
            const uint8_t a = px[3];
            dst_row[x] = pack_rgba(r, g, b, a);
        }
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

    ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height, WINDOW_FORMAT_RGBA_8888);

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int width = std::min(frame.width, buffer.width);
    const int height = std::min(frame.height, buffer.height);

    for (int y = 0; y < height; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* y_row = frame.planes[0].data + y * frame.planes[0].stride;
        const uint8_t* u_row = frame.planes[1].data + (y / 2) * frame.planes[1].stride;
        const uint8_t* v_row = frame.planes[2].data + (y / 2) * frame.planes[2].stride;
        for (int x = 0; x < width; ++x) {
            const int yy = static_cast<int>(y_row[x]);
            const int uu = static_cast<int>(u_row[x / 2]) - 128;
            const int vv = static_cast<int>(v_row[x / 2]) - 128;
            const uint8_t r = clamp_to_u8(yy + ((359 * vv) >> 8));
            const uint8_t g = clamp_to_u8(yy - ((88 * uu + 183 * vv) >> 8));
            const uint8_t b = clamp_to_u8(yy + ((454 * uu) >> 8));
            dst_row[x] = pack_rgba(r, g, b);
        }
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

}  // namespace streambridge::android
