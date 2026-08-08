#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "streambridge/capture.h"
#include "../ffmpeg_utils.h"

namespace streambridge {

// ============================================================
// V4L2VideoCapture — 原生 V4L2 视频采集（MMAP 模式）
// ============================================================
// 支持 MJPG（通过 FFmpeg MJPEG 解码）和 YUYV（通过 swscale 转 YUV420P）。
// 线程安全：open/start/stop/close 由控制线程串行调用，
//           回调在采集线程中同步执行。
// ============================================================
class V4L2VideoCapture : public IVideoCapture {
public:
    V4L2VideoCapture();
    ~V4L2VideoCapture() override;

    Result<void> open(const VideoCaptureConfig& config) override;
    Result<void> start(VideoFrameCallback on_frame,
                       CaptureErrorCallback on_error) override;
    void stop() override;
    void close() override;

    std::vector<VideoCaptureCapability> capabilities() const override;
    VideoCaptureConfig current_config() const override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return running_.load(); }

private:
    void capture_loop();
    bool init_mmap();
    void cleanup_mmap();
    Result<void> init_decoder(uint32_t pixelformat);  // MJPEG → YUV420P

    int fd_ = -1;                      // V4L2 设备文件描述符
    VideoCaptureConfig config_;

    // MMAP 缓冲
    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers_;

    // MJPEG 解码器（仅 MJPG 模式使用）
    AVCodecContextPtr mjpeg_dec_;
    AVPacketPtr mjpeg_pkt_;
    AVFramePtr mjpeg_frame_;
    SwsContextPtr sws_;                // YUYV → YUV420P 转换
    PixelFormat src_pix_fmt_ = PixelFormat::Unknown;

    VideoFrameCallback on_frame_;
    CaptureErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    bool is_open_ = false;
    int target_fps_ = 30;
};

}  // namespace streambridge
