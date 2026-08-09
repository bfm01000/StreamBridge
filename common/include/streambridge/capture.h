#pragma once
// 采集接口

#include <functional>
#include <string>
#include <vector>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// 回调类型
// ============================================================
using VideoFrameCallback = std::function<void(VideoFrame frame)>;
using AudioFrameCallback = std::function<void(AudioFrame frame)>;
using CaptureErrorCallback = std::function<void(ErrorDomain, ErrorCode, std::string)>;

// ============================================================
// 采集能力
// ============================================================
// 视频采集设备能力：设备标识、支持的分辨率/像素格式/帧率列表
struct VideoCaptureCapability {
    std::string device_id;
    std::string device_path;
    struct Resolution { int width; int height; };
    std::vector<Resolution> resolutions;
    std::vector<PixelFormat> formats;
    std::vector<int> frame_rates;
};

// 音频采集设备能力：支持的采样率/声道数/采样格式列表
struct AudioCaptureCapability {
    std::string device_id;
    std::string device_path;
    std::vector<int> sample_rates;
    std::vector<int> channels;
    std::vector<SampleFormat> formats;
};

// ============================================================
// 采集配置
// ============================================================
// 视频采集配置：输入源、目标分辨率/帧率/像素格式
struct VideoCaptureConfig {
    std::string source;  // 文件路径 / lavfi 描述 / V4L2 设备路径
    int target_width = 1280;
    int target_height = 720;
    int target_fps = 30;
    PixelFormat target_format = PixelFormat::YUV420P;
    bool loop = false;
    bool no_throttle = false;  // 跳过实时节流（基准测试用）
};

// 音频采集配置：输入源、目标采样率/声道数/采样格式/帧大小
struct AudioCaptureConfig {
    std::string source;
    int target_sample_rate = 48000;
    int target_channels = 2;
    SampleFormat target_format = SampleFormat::FLTPlanar;
    int target_frame_size = 1024;
    bool loop = false;
    bool no_throttle = false;
};

// ============================================================
// 采集接口
// ============================================================
// 视频采集抽象接口：open/start/stop/close，通过回调输出 VideoFrame
class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;

    virtual Result<void> open(const VideoCaptureConfig& config) = 0;
    virtual Result<void> start(VideoFrameCallback on_frame,
                               CaptureErrorCallback on_error) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual std::vector<VideoCaptureCapability> capabilities() const = 0;
    virtual VideoCaptureConfig current_config() const = 0;

    virtual bool is_open() const = 0;
    virtual bool is_running() const = 0;
};

// 音频采集抽象接口：open/start/stop/close，通过回调输出 AudioFrame
class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    virtual Result<void> open(const AudioCaptureConfig& config) = 0;
    virtual Result<void> start(AudioFrameCallback on_frame,
                               CaptureErrorCallback on_error) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual std::vector<AudioCaptureCapability> capabilities() const = 0;
    virtual AudioCaptureConfig current_config() const = 0;

    virtual bool is_open() const = 0;
    virtual bool is_running() const = 0;
};

}  // namespace streambridge
