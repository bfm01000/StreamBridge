#pragma once

#include <android/hardware_buffer.h>

#include <cstdint>
#include <memory>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

struct HardwareBufferFrameDesc {
    int width = 0;
    int height = 0;
    uint32_t format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    int layers = 1;
    streambridge::PixelFormat pixel_format = streambridge::PixelFormat::RGBA;
};

class HardwareBufferFrameBuffer final
        : public streambridge::FrameBuffer
        , public std::enable_shared_from_this<HardwareBufferFrameBuffer> {
public:
    HardwareBufferFrameBuffer(AHardwareBuffer* buffer,
                              int width,
                              int height,
                              int stride_pixels,
                              uint32_t format,
                              streambridge::PixelFormat pixel_format);
    ~HardwareBufferFrameBuffer() override;

    HardwareBufferFrameBuffer(const HardwareBufferFrameBuffer&) = delete;
    HardwareBufferFrameBuffer& operator=(const HardwareBufferFrameBuffer&) = delete;

    static streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>
    allocate(const HardwareBufferFrameDesc& desc);

    static streambridge::Result<std::shared_ptr<HardwareBufferFrameBuffer>>
    wrap(AHardwareBuffer* buffer, streambridge::PixelFormat pixel_format);

    uint8_t* data() override { return locked_data_; }
    const uint8_t* data() const override { return locked_data_; }
    size_t size() const override { return size_; }
    streambridge::MemoryType memory_type() const override {
        return streambridge::MemoryType::HardwareBuffer;
    }

    AHardwareBuffer* buffer() const { return buffer_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int stride_pixels() const { return stride_pixels_; }
    int stride_bytes() const;
    uint32_t format() const { return format_; }
    streambridge::PixelFormat pixel_format() const { return pixel_format_; }
    bool is_locked() const { return locked_data_ != nullptr; }

    streambridge::Result<void> lock(uint64_t usage);
    void unlock();
    streambridge::VideoFrame to_video_frame(streambridge::TimePointUs pts,
                                            streambridge::TimeDeltaUs duration);

private:
    static int bytes_per_pixel(uint32_t format);

    AHardwareBuffer* buffer_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_pixels_ = 0;
    uint32_t format_ = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    streambridge::PixelFormat pixel_format_ = streambridge::PixelFormat::RGBA;
    uint8_t* locked_data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace streambridge::android
