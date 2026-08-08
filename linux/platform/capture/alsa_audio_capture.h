#pragma once

#include <atomic>
#include <thread>
#include <vector>

// ALSA 头文件（snd_pcm_t 是 typedef，无法前向声明）
#include <alsa/asoundlib.h>

#include "streambridge/capture.h"

namespace streambridge {

// ============================================================
// ALSAAudioCapture — 原生 ALSA 音频采集
// ============================================================
// 使用阻塞 readi 模式在独立线程中运行。
// 线程安全：open/start/stop/close 由控制线程串行调用，
//           回调在采集线程中同步执行。
// ============================================================
class ALSAAudioCapture : public IAudioCapture {
public:
    ALSAAudioCapture();
    ~ALSAAudioCapture() override;

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
    snd_pcm_t* pcm_ = nullptr;         // ALSA PCM 句柄（不透明）
    AudioFrameCallback on_frame_;
    CaptureErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    bool is_open_ = false;
};

}  // namespace streambridge
