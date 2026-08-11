#pragma once
// 编解码接口

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// 编码能力
// ============================================================
// 编码器能力：编码格式、硬件加速标记、输入像素/采样格式与码率范围
struct CodecCapability {
    CodecId codec = CodecId::Unknown;
    bool is_hardware = false;
    std::string hardware_name;
    std::vector<PixelFormat> input_pixel_formats;
    std::vector<SampleFormat> input_sample_formats;
    // 码率范围
    int64_t min_bitrate = 0;
    int64_t max_bitrate = 0;
};

// 解码器能力：解码格式、硬件加速标记与输出像素/采样格式
struct DecodeCapability {
    CodecId codec = CodecId::Unknown;
    bool is_hardware = false;
    std::string hardware_name;
    std::vector<PixelFormat> output_pixel_formats;
    std::vector<SampleFormat> output_sample_formats;
};

// ============================================================
// 编码/解码配置
// ============================================================
// 视频编码配置：H.264 参数（分辨率、帧率、码率、GOP、profile）
struct VideoEncodeConfig {
    CodecId codec = CodecId::H264;
    int width = 1280;
    int height = 720;
    double frame_rate = 30.0;
    int bitrate_bps = 2'000'000;
    int gop_size = 60;          // 2s @ 30fps
    int b_frames = 0;
    std::string preset = "veryfast";
    std::string tune = "zerolatency";
    std::string profile = "baseline";
    PixelFormat input_format = PixelFormat::YUV420P;
    int thread_count = 2;
};

// 音频编码配置：AAC 参数（采样率、声道、码率、帧大小）
struct AudioEncodeConfig {
    CodecId codec = CodecId::AAC;
    int sample_rate = 48000;
    int channels = 2;
    SampleFormat input_format = SampleFormat::FLTPlanar;
    int bitrate_bps = 128'000;
    int frame_size = 1024;
};

// 视频解码配置：解码格式、线程数与输出像素格式
struct VideoDecodeConfig {
    CodecId codec = CodecId::H264;
    std::optional<int> thread_count;
    PixelFormat output_format = PixelFormat::YUV420P;
};

// 音频解码配置：解码格式与输出采样格式
struct AudioDecodeConfig {
    CodecId codec = CodecId::AAC;
    SampleFormat output_format = SampleFormat::S16;
};

// ============================================================
// 编码器接口
// ============================================================
// 视频编码器抽象接口：送原始帧 → 输出编码 MediaPacket
    // 视频编码抽象接口：编码/刷新/关闭，输出 MediaPacket 列表
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual Result<void> open(const VideoEncodeConfig& config) = 0;
    // 送一帧，返回编码后的 packets（可能为空）
    virtual Result<std::vector<MediaPacket>> encode(VideoFrame frame) = 0;
    // 刷新编码器缓冲
    virtual Result<std::vector<MediaPacket>> drain() = 0;
    virtual void close() = 0;

    virtual CodecCapability capability() const = 0;
    // extradata（SPS/PPS for H.264），open 后即可调用
    virtual std::vector<uint8_t> extradata() const = 0;

    virtual bool is_open() const = 0;
};

// 音频编码抽象接口：编码/刷新/关闭，输出 MediaPacket 列表
class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    virtual Result<void> open(const AudioEncodeConfig& config) = 0;
    virtual Result<std::vector<MediaPacket>> encode(AudioFrame frame) = 0;
    virtual Result<std::vector<MediaPacket>> drain() = 0;
    virtual void close() = 0;

    virtual CodecCapability capability() const = 0;
    virtual std::vector<uint8_t> extradata() const = 0;

    virtual bool is_open() const = 0;
};

// ============================================================
// 解码器接口 v2.1
// ============================================================

// 解码状态：帧就绪 / 需更多输入 / 流结束
enum class DecodeStatus {
    FrameReady,   // 成功取出一帧
    TryAgain,     // 暂无输出，需要更多输入（EAGAIN / timeout）
    EndOfStream,  // 流结束
};

// 解码器能力：是否硬件解码、是否支持 Surface/CPU 输出
struct DecoderCapability {
    bool hardware = false;
    bool supports_surface_output = false;
    bool supports_cpu_output = false;
};

// ── Frame handles（标记联合，编译期保证只有一个分支有效）──

// CPU 帧：shared_ptr owning，Session/Renderer 持有最后一个引用时自动释放
struct CpuFrameHandle {
    std::shared_ptr<CpuFrameBuffer> buffer;
    VideoFrame frame;
};

// Surface 输出：opaque，Session 不需要知道 ANativeWindow*
struct DecoderSurfaceHandle {};

// 未来：DMA-BUF、GPU Texture
struct DmaBufFrameHandle { /* fd, planes, stride, modifier */ };
struct GpuTextureHandle { /* texture_id */ };

using FramePayload = std::variant<
    CpuFrameHandle,
    DecoderSurfaceHandle,
    DmaBufFrameHandle,
    GpuTextureHandle
>;

// 统一解码输出
struct DecodeOutput {
    uint64_t frame_id = 0;
    int64_t pts_us = 0;
    FramePayload payload;
};

// 视频解码器统一接口
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // --- 生命周期 ---
    virtual Result<void> open(const StreamInfo& info) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // --- 数据路径 ---
    virtual Result<DecodeStatus> send_packet(const MediaPacket& packet) = 0;
    virtual Result<DecodeOutput> receive_frame(int timeout_ms) = 0;

    // --- 帧生命周期 ---
    // 向显示目标提交帧（DecoderSurface / DMA-BUF / Texture 用）
    virtual Result<void> present_frame(uint64_t frame_id, int64_t target_time_ns) = 0;
    // 丢弃帧（所有模式通用）
    virtual Result<void> discard_frame(uint64_t frame_id) = 0;

    // --- 控制 ---
    virtual Result<void> drain() = 0;
    virtual void flush() = 0;

    // --- 能力 ---
    virtual DecoderCapability capability() const = 0;
};

// 音频解码器接口（本次不变）
class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual Result<void> open(const StreamInfo& info) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual Result<void> send_packet(const MediaPacket& packet) = 0;

    struct DecodeResult {
        bool has_frame = false;
        AudioFrame frame;
    };
    virtual Result<DecodeResult> receive_frame() = 0;

    virtual Result<void> drain() = 0;
    virtual void flush() = 0;

    virtual DecodeCapability capability() const = 0;
};

}  // namespace streambridge
