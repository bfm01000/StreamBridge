#pragma once
// StreamBridge common types — 时间、枚举、媒体数据结构
// 平台无关，不依赖 FFmpeg/NDK/JNI/ALSA/V4L2

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace streambridge {

// ============================================================
// 1. 时间类型（公共层统一微秒）
// ============================================================

struct TimeDeltaUs;

// 时间点类型：单调时钟微秒时间戳，公共层统一时间基
struct TimePointUs {
    int64_t us = 0;  // microseconds since monotonic epoch

    static TimePointUs from_ms(int64_t ms) { return {ms * 1000}; }
    static TimePointUs from_seconds(double s) {
        return {static_cast<int64_t>(s * 1'000'000.0)};
    }
    static TimePointUs zero() { return {0}; }

    int64_t to_ms() const { return us / 1000; }
    double to_seconds() const { return static_cast<double>(us) / 1'000'000.0; }

    TimePointUs operator+(TimeDeltaUs delta) const;
    TimePointUs operator-(TimeDeltaUs delta) const;
    TimeDeltaUs operator-(TimePointUs other) const;

    bool operator==(TimePointUs other) const { return us == other.us; }
    bool operator!=(TimePointUs other) const { return us != other.us; }
    bool operator<(TimePointUs other) const { return us < other.us; }
    bool operator<=(TimePointUs other) const { return us <= other.us; }
    bool operator>(TimePointUs other) const { return us > other.us; }
    bool operator>=(TimePointUs other) const { return us >= other.us; }
};

// 时间间隔类型：微秒时长，提供帧/采样/毫秒换算
struct TimeDeltaUs {
    int64_t us = 0;

    static TimeDeltaUs from_ms(int64_t ms) { return {ms * 1000}; }
    static TimeDeltaUs from_seconds(double s) {
        return {static_cast<int64_t>(s * 1'000'000.0)};
    }
    static TimeDeltaUs from_frames(int64_t frames, double fps) {
        return {static_cast<int64_t>(frames * 1'000'000.0 / fps)};
    }
    static TimeDeltaUs from_samples(int64_t samples, int sample_rate) {
        return {samples * 1'000'000 / sample_rate};
    }
    static TimeDeltaUs zero() { return {0}; }

    int64_t to_ms() const { return us / 1000; }
    double to_seconds() const { return static_cast<double>(us) / 1'000'000.0; }
    int64_t to_samples(int sample_rate) const {
        return us * sample_rate / 1'000'000;
    }

    TimeDeltaUs operator+(TimeDeltaUs other) const {
        return {us + other.us};
    }
    TimeDeltaUs operator-(TimeDeltaUs other) const {
        return {us - other.us};
    }
    TimeDeltaUs operator*(double scale) const {
        return {static_cast<int64_t>(us * scale)};
    }
    bool operator==(TimeDeltaUs other) const { return us == other.us; }
    bool operator!=(TimeDeltaUs other) const { return us != other.us; }
    bool operator<(TimeDeltaUs other) const { return us < other.us; }
    bool operator<=(TimeDeltaUs other) const { return us <= other.us; }
    bool operator>(TimeDeltaUs other) const { return us > other.us; }
    bool operator>=(TimeDeltaUs other) const { return us >= other.us; }
};

inline TimePointUs TimePointUs::operator+(TimeDeltaUs delta) const {
    return {us + delta.us};
}
inline TimePointUs TimePointUs::operator-(TimeDeltaUs delta) const {
    return {us - delta.us};
}
inline TimeDeltaUs TimePointUs::operator-(TimePointUs other) const {
    return {us - other.us};
}

// ============================================================
// 2. 有理数 time base
// ============================================================

// 有理数 time base：num/den 表示 PTS 换算的时间基
struct Rational {
    int32_t num = 1;
    int32_t den = 1;

    double to_double() const { return den != 0 ? static_cast<double>(num) / den : 0.0; }
    static Rational micros() { return {1, 1'000'000}; }
    static Rational millis() { return {1, 1'000}; }
};

// ============================================================
// 3. 媒体枚举
// ============================================================

enum class MediaType { Unknown = 0, Audio = 1, Video = 2 };

enum class CodecId {
    Unknown = 0,
    H264    = 1,
    AAC     = 2,
    H265    = 3,
    Opus    = 4,
};

enum class PixelFormat {
    Unknown  = 0,
    YUV420P  = 1,
    NV12     = 2,
    NV21     = 3,
    YUV422P  = 4,
    YUYV422  = 5,
    BGRA     = 6,
    RGBA     = 7,
    RGB24    = 8,
    GRAY8    = 9,
};

enum class SampleFormat {
    Unknown    = 0,
    S16        = 1,
    S16Planar  = 2,
    FLT        = 3,
    FLTPlanar  = 4,
};

enum class MemoryType : uint8_t {
    CPU       = 0,   // CpuFrameBuffer
    GLTexture = 1,   // OpenGL texture (future)
    DMA       = 2,   // DMA-BUF fd (future)
};

// ============================================================
// 4. FrameBuffer — 底层内存所有权抽象
// ============================================================

// 帧内存所有权抽象：统一管理媒体帧底层内存的分配与释放
class FrameBuffer {
public:
    virtual ~FrameBuffer() = default;

    virtual uint8_t* data() = 0;
    virtual const uint8_t* data() const = 0;
    virtual size_t size() const = 0;
    virtual MemoryType memory_type() const { return MemoryType::CPU; }
};

// CPU 堆内存实现：一次性分配连续内存，所有 Plane 共享
class CpuFrameBuffer final : public FrameBuffer {
public:
    explicit CpuFrameBuffer(size_t sz)
        : data_(std::make_unique<uint8_t[]>(sz)), size_(sz) {}

    uint8_t* data() override { return data_.get(); }
    const uint8_t* data() const override { return data_.get(); }
    size_t size() const override { return size_; }

private:
    std::unique_ptr<uint8_t[]> data_;
    size_t size_ = 0;
};

// ============================================================
// 5. Plane — 纯视图，不持有内存
// ============================================================

// 平面视图：描述帧内某一平面的偏移/大小/步长，不持有内存
struct Plane {
    uint8_t* data = nullptr;   // 此 plane 起始地址
    size_t size = 0;           // 此 plane 字节数
    int stride = 0;            // 行步长（字节）
    size_t offset = 0;         // 相对 FrameBuffer::data() 的偏移
};

// ============================================================
// 6. VideoFrame / AudioFrame — 数据帧
// ============================================================

// 编码后的媒体包：类型/编码格式/PTS/DTS 与码流数据
struct MediaPacket {
    MediaType type = MediaType::Unknown;
    CodecId codec = CodecId::Unknown;
    TimePointUs pts{0};
    TimePointUs dts{0};
    TimeDeltaUs duration{0};
    bool is_key_frame = false;
    std::vector<uint8_t> data;

    bool has_codec_config = false;
    std::vector<uint8_t> codec_config;

    int stream_index = -1;
    int64_t sequence_number = 0;

    bool is_video() const { return type == MediaType::Video; }
    bool is_audio() const { return type == MediaType::Audio; }
    bool has_valid_pts() const { return pts.us >= 0; }
};

// 原始视频帧：Plane 视图 + buffer 统一持有内存，携带格式与 PTS
struct VideoFrame {
    PixelFormat format = PixelFormat::Unknown;
    int width = 0;
    int height = 0;
    TimePointUs pts{0};
    TimeDeltaUs duration{0};

    static constexpr int kMaxPlanes = 4;
    Plane planes[kMaxPlanes];
    int num_planes = 0;

    std::shared_ptr<FrameBuffer> buffer;  // ★ 统一生命周期

    int64_t frame_index = 0;

    MemoryType memory_type() const {
        return buffer ? buffer->memory_type() : MemoryType::CPU;
    }

    bool is_valid() const {
        return buffer != nullptr && num_planes > 0 && width > 0 && height > 0;
    }
};

// 原始音频帧：Plane 视图 + buffer 统一持有内存，携带采样信息与 PTS
struct AudioFrame {
    SampleFormat format = SampleFormat::Unknown;
    int sample_rate = 0;
    int channels = 0;
    int num_samples = 0;
    TimePointUs pts{0};
    TimeDeltaUs duration{0};

    static constexpr int kMaxPlanes = 8;
    Plane planes[kMaxPlanes];
    int num_planes = 0;

    std::shared_ptr<FrameBuffer> buffer;  // ★ 统一生命周期

    int64_t frame_index = 0;

    int bytes_per_sample() const {
        switch (format) {
            case SampleFormat::S16:       return 2;
            case SampleFormat::S16Planar: return 2;
            case SampleFormat::FLT:       return 4;
            case SampleFormat::FLTPlanar: return 4;
            default: return 0;
        }
    }
    int total_samples() const { return num_samples * channels; }
    TimeDeltaUs duration_us() const {
        return TimeDeltaUs::from_samples(num_samples, sample_rate);
    }
    bool is_valid() const {
        return buffer != nullptr && num_planes > 0 && num_samples > 0;
    }
};

// 流元数据：媒体类型、编解码参数、时间基与扩展数据
struct StreamInfo {
    MediaType type = MediaType::Unknown;
    CodecId codec = CodecId::Unknown;

    int width = 0;
    int height = 0;
    double frame_rate = 0.0;
    PixelFormat pixel_format = PixelFormat::Unknown;

    int sample_rate = 0;
    int channels = 0;
    SampleFormat sample_format = SampleFormat::Unknown;

    std::vector<uint8_t> codec_extradata;

    Rational time_base{1, 1'000'000};
    int64_t bitrate_bps = 0;

    bool is_video() const { return type == MediaType::Video; }
    bool is_audio() const { return type == MediaType::Audio; }
};

}  // namespace streambridge
