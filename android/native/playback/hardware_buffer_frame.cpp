#include "hardware_buffer_frame.h"

#include <algorithm>

#include "streambridge/logging.h"

namespace streambridge::android {
namespace {

constexpr char kLogTag[] = "StreamBridgeAHB";

streambridge::ErrorCode ahb_error_code(int rc) {
    return rc == 0 ? streambridge::ErrorCode::Ok : streambridge::ErrorCode::DeviceBusy;
}

}  // namespace

HardwareBufferFrameBuffer::HardwareBufferFrameBuffer(
        AHardwareBuffer* buffer,
        int width,
        int height,
        int stride_pixels,
        uint32_t format,
        streambridge::PixelFormat pixel_format)
    : buffer_(buffer)
    , width_(width)
    , height_(height)
    , stride_pixels_(stride_pixels)
    , format_(format)
    , pixel_format_(pixel_format) {
    if (buffer_ != nullptr) {
        AHardwareBuffer_acquire(buffer_);
    }
    const int bpp = bytes_per_pixel(format_);
    if (bpp > 0 && stride_pixels_ > 0 && height_ > 0) {
        size_ = static_cast<size_t>(stride_pixels_) *
                static_cast<size_t>(height_) *
                static_cast<size_t>(bpp);
    }
}

HardwareBufferFrameBuffer::~HardwareBufferFrameBuffer() {
    unlock();
    if (buffer_ != nullptr) {
        AHardwareBuffer_release(buffer_);
        buffer_ = nullptr;
    }
}

streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>
HardwareBufferFrameBuffer::allocate(const HardwareBufferFrameDesc& desc) {
    if (desc.width <= 0 || desc.height <= 0 || desc.layers <= 0) {
        return streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>::err(
            streambridge::ErrorDomain::Config,
            streambridge::ErrorCode::InvalidArgument,
            "invalid AHardwareBuffer dimensions");
    }

    AHardwareBuffer_Desc ahb_desc{};
    ahb_desc.width = static_cast<uint32_t>(desc.width);
    ahb_desc.height = static_cast<uint32_t>(desc.height);
    ahb_desc.layers = static_cast<uint32_t>(desc.layers);
    ahb_desc.format = desc.format;
    ahb_desc.usage = desc.usage;

    AHardwareBuffer* buffer = nullptr;
    const int rc = AHardwareBuffer_allocate(&ahb_desc, &buffer);
    if (rc != 0 || buffer == nullptr) {
        return streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>::err(
            streambridge::ErrorDomain::Device,
            ahb_error_code(rc),
            "AHardwareBuffer_allocate failed");
    }

    auto wrapped = wrap(buffer, desc.pixel_format);
    AHardwareBuffer_release(buffer);
    return wrapped;
}

streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>
HardwareBufferFrameBuffer::wrap(AHardwareBuffer* buffer,
                                streambridge::PixelFormat pixel_format) {
    if (buffer == nullptr) {
        return streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>::err(
            streambridge::ErrorDomain::Config,
            streambridge::ErrorCode::InvalidArgument,
            "AHardwareBuffer is null");
    }

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    if (desc.width == 0 || desc.height == 0 || desc.stride == 0) {
        return streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>::err(
            streambridge::ErrorDomain::Device,
            streambridge::ErrorCode::InvalidState,
            "AHardwareBuffer has invalid geometry");
    }

    auto frame_buffer = std::make_shared<HardwareBufferFrameBuffer>(
        buffer,
        static_cast<int>(desc.width),
        static_cast<int>(desc.height),
        static_cast<int>(desc.stride),
        desc.format,
        pixel_format);
    SB_LOG_I(kLogTag, "wrapped AHardwareBuffer %dx%d stride=%d fmt=%u",
             frame_buffer->width(), frame_buffer->height(),
             frame_buffer->stride_pixels(), frame_buffer->format());
    return streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>::ok(frame_buffer);
}

int HardwareBufferFrameBuffer::stride_bytes() const {
    const int bpp = bytes_per_pixel(format_);
    if (bpp <= 0 || stride_pixels_ <= 0) {
        return 0;
    }
    return stride_pixels_ * bpp;
}

streambridge::Result<void> HardwareBufferFrameBuffer::lock(uint64_t usage) {
    if (buffer_ == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Device,
            streambridge::ErrorCode::InvalidState,
            "AHardwareBuffer is null");
    }
    if (locked_data_ != nullptr) {
        return streambridge::Result<void>::ok();
    }

    void* data = nullptr;
    const int rc = AHardwareBuffer_lock(buffer_, usage, -1, nullptr, &data);
    if (rc != 0 || data == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Device,
            ahb_error_code(rc),
            "AHardwareBuffer_lock failed");
    }
    locked_data_ = static_cast<uint8_t*>(data);
    return streambridge::Result<void>::ok();
}

void HardwareBufferFrameBuffer::unlock() {
    if (buffer_ == nullptr || locked_data_ == nullptr) {
        return;
    }
    AHardwareBuffer_unlock(buffer_, nullptr);
    locked_data_ = nullptr;
}

streambridge::VideoFrame HardwareBufferFrameBuffer::to_video_frame(
        streambridge::TimePointUs pts,
        streambridge::TimeDeltaUs duration) {
    streambridge::VideoFrame frame;
    frame.format = pixel_format_;
    frame.width = width_;
    frame.height = height_;
    frame.pts = pts;
    frame.duration = duration;
    frame.buffer = shared_from_this();
    frame.num_planes = 1;
    frame.planes[0].data = locked_data_;
    frame.planes[0].stride = stride_bytes();
    frame.planes[0].size = size_;
    frame.planes[0].offset = 0;
    return frame;
}

int HardwareBufferFrameBuffer::bytes_per_pixel(uint32_t format) {
    switch (format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
            return 4;
        case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            return 3;
        case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
            return 2;
        default:
            return 0;
    }
}

}  // namespace streambridge::android
