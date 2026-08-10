#pragma once
// 编解码接口

#include <optional>
#include <string>
#include <vector>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// 编码能力
// ============================================================
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

struct AudioEncodeConfig {
    CodecId codec = CodecId::AAC;
    int sample_rate = 48000;
    int channels = 2;
    SampleFormat input_format = SampleFormat::FLTPlanar;
    int bitrate_bps = 128'000;
    int frame_size = 1024;
};

struct VideoDecodeConfig {
    CodecId codec = CodecId::H264;
    std::optional<int> thread_count;
    PixelFormat output_format = PixelFormat::YUV420P;
};

struct AudioDecodeConfig {
    CodecId codec = CodecId::AAC;
    SampleFormat output_format = SampleFormat::S16;
};

// ============================================================
// 编码器接口
// ============================================================
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
// 解码器接口
// ============================================================

// 解码器输出信息（不包含像素/采样数据——零拷贝友好）
struct DecodeOutputInfo {
    bool has_output = false;
    int64_t pts_us = -1;
    int64_t duration_us = -1;
    int output_index = -1;  // decoder-specific handle for release_output
};

// 视频解码器统一接口
// FFmpeg 软解和 MediaCodec 硬解均实现此接口
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    enum class OutputMode {
        CpuFrame,   // 解码帧落在 CPU 内存，通过 receive_frame() 取出
        Surface,    // 解码帧直接渲染到 Surface，零拷贝
    };

    // --- 生命周期 ---
    virtual Result<void> open(const StreamInfo& info) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // --- 数据路径（两个模式通用）---
    // 喂入编码包；可多次调用，解码器内部缓冲
    virtual Result<void> send_packet(const MediaPacket& packet) = 0;

    // 取出下一帧的输出信息（阻塞或超时返回 has_output=false）
    virtual Result<DecodeOutputInfo> dequeue_output(int64_t timeout_us) = 0;

    // 释放/渲染输出帧
    // CpuFrame 模式：render 参数无意义（帧已通过 receive_frame 取出）
    // Surface 模式：render=true 提交到 Surface，render=false 丢弃
    virtual void release_output(int output_index, bool render) = 0;

    // 冲刷解码器缓冲（发送 null packet，取回残余帧）
    virtual Result<void> drain() = 0;

    // 清空内部缓冲（用于 seek / reconnect）
    virtual void flush() = 0;

    // --- 能力查询 ---
    virtual OutputMode output_mode() const = 0;
    virtual DecodeCapability capability() const = 0;

    // --- CPU 模式专用 ---
    struct CpuFrameResult {
        bool has_frame = false;
        VideoFrame frame;
    };
    // 从 output_index 取出解码帧数据（调用后应 release_output(index, false)）
    virtual Result<CpuFrameResult> receive_frame(int output_index) = 0;
};

// 音频解码器接口
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
