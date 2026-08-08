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
// 解码器接口（播放端用，M4 实现）
// ============================================================
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual Result<void> open(const VideoDecodeConfig& config,
                              const StreamInfo& stream_info) = 0;
    virtual Result<std::vector<VideoFrame>> decode(MediaPacket packet) = 0;
    virtual Result<std::vector<VideoFrame>> drain() = 0;
    virtual void flush() = 0;
    virtual void close() = 0;

    virtual DecodeCapability capability() const = 0;
    virtual bool is_open() const = 0;
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual Result<void> open(const AudioDecodeConfig& config,
                              const StreamInfo& stream_info) = 0;
    virtual Result<std::vector<AudioFrame>> decode(MediaPacket packet) = 0;
    virtual Result<std::vector<AudioFrame>> drain() = 0;
    virtual void flush() = 0;
    virtual void close() = 0;

    virtual DecodeCapability capability() const = 0;
    virtual bool is_open() const = 0;
};

}  // namespace streambridge
