#pragma once

#include <atomic>
#include <thread>

#include "streambridge/capture.h"
#include "../ffmpeg_utils.h"

namespace streambridge {

class FFmpegVideoCapture : public IVideoCapture {
public:
    FFmpegVideoCapture();
    ~FFmpegVideoCapture() override;

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

    VideoCaptureConfig config_;
    AVFormatContextPtr fmt_ctx_;
    AVCodecContextPtr dec_ctx_;
    int video_stream_idx_ = -1;

    VideoFrameCallback on_frame_;
    CaptureErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    bool is_open_ = false;
};

}  // namespace streambridge
