#pragma once

#include <atomic>
#include <thread>

#include "streambridge/capture.h"
#include "../ffmpeg_utils.h"

namespace streambridge {

class FFmpegAudioCapture : public IAudioCapture {
public:
    FFmpegAudioCapture();
    ~FFmpegAudioCapture() override;

    Result<void> open(const AudioCaptureConfig& config) override;
    Result<void> start(AudioFrameCallback on_frame,
                       CaptureErrorCallback on_error) override;
    void stop() override;
    void close() override;

    std::vector<AudioCaptureCapability> capabilities() const override;
    AudioCaptureConfig current_config() const override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return running_.load(); }

private:
    void capture_loop();

    AudioCaptureConfig config_;
    AVFormatContextPtr fmt_ctx_;
    AVCodecContextPtr dec_ctx_;
    int audio_stream_idx_ = -1;

    AudioFrameCallback on_frame_;
    CaptureErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    bool is_open_ = false;
};

}  // namespace streambridge
