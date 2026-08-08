#include "alsa_audio_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <alsa/asoundlib.h>

#include "../logger.h"

namespace streambridge {

// ============================================================
// 创建/析构
// ============================================================

ALSAAudioCapture::ALSAAudioCapture() = default;

ALSAAudioCapture::~ALSAAudioCapture() {
    close();
}

// ============================================================
// 生命周期
// ============================================================

Result<void> ALSAAudioCapture::open(const AudioCaptureConfig& config) {
    config_ = config;

    const char* device = config.source.empty() ? "default" : config.source.c_str();
    // 常见的便捷别名: "hw:0,0" (硬件设备), "plughw:0,0" (带自动转换), "default" (系统默认)

    int err = snd_pcm_open(&pcm_, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        return Result<void>::err(
            ErrorDomain::Device, ErrorCode::DeviceNotFound,
            std::string("snd_pcm_open(") + device + ") failed: " + snd_strerror(err));
    }

    // 设置硬件参数
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_, hw_params);

    // 交错访问（S16_LE 的标准模式）
    snd_pcm_hw_params_set_access(pcm_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

    // 采样格式: 优先 S16_LE（ALSA 最常见格式），编码器的 swr 会做后续转换
    snd_pcm_format_t alsa_fmt = SND_PCM_FORMAT_S16_LE;
    err = snd_pcm_hw_params_set_format(pcm_, hw_params, alsa_fmt);
    if (err < 0) {
        // 回退：尝试其他常见格式
        alsa_fmt = SND_PCM_FORMAT_S32_LE;
        err = snd_pcm_hw_params_set_format(pcm_, hw_params, alsa_fmt);
    }
    if (err < 0) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return Result<void>::err(
            ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
            std::string("ALSA format not supported: ") + snd_strerror(err));
    }

    // 采样率: 接近目标即可（ALSA 会选最接近的）
    unsigned int rate = static_cast<unsigned int>(config.target_sample_rate);
    unsigned int rate_orig = rate;
    snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &rate, nullptr);
    if (rate != rate_orig) {
        LOG_W("alsa", "sample rate adjusted: %u -> %u Hz", rate_orig, rate);
    }

    // 声道数
    unsigned int channels = static_cast<unsigned int>(config.target_channels);
    snd_pcm_hw_params_set_channels_near(pcm_, hw_params, &channels);

    // 缓冲: 约 100ms
    snd_pcm_uframes_t buffer_size = rate * 100 / 1000;  // 100ms
    snd_pcm_hw_params_set_buffer_size_near(pcm_, hw_params, &buffer_size);

    // 周期: 约 20ms
    snd_pcm_uframes_t period_size = rate * 20 / 1000;
    unsigned int periods = 4;
    snd_pcm_hw_params_set_period_size_near(pcm_, hw_params, &period_size, nullptr);
    snd_pcm_hw_params_set_periods_near(pcm_, hw_params, &periods, nullptr);

    // 应用参数
    err = snd_pcm_hw_params(pcm_, hw_params);
    if (err < 0) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return Result<void>::err(
            ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
            std::string("snd_pcm_hw_params failed: ") + snd_strerror(err));
    }

    // 读回实际参数
    snd_pcm_hw_params_get_rate(hw_params, &rate, nullptr);
    snd_pcm_hw_params_get_channels(hw_params, &channels);
    snd_pcm_hw_params_get_period_size(hw_params, &period_size, nullptr);
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);

    // 更新配置以反映实际参数
    config_.target_sample_rate = static_cast<int>(rate);
    config_.target_channels = static_cast<int>(channels);
    config_.target_format = SampleFormat::S16;     // ALSA 输出 S16
    config_.target_frame_size = static_cast<int>(period_size);

    LOG_I("alsa", "opened %s: %u Hz, %u ch, S16_LE, "
          "period=%lu frames, buffer=%lu frames",
          device, rate, channels, period_size, buffer_size);

    is_open_ = true;
    return Result<void>::ok();
}

Result<void> ALSAAudioCapture::start(AudioFrameCallback on_frame,
                                     CaptureErrorCallback on_error) {
    if (!is_open_) {
        return Result<void>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Capture not open");
    }
    on_frame_ = std::move(on_frame);
    on_error_ = std::move(on_error);
    stop_requested_ = false;
    running_ = true;

    // 准备 ALSA（切换到 PREPARED 状态）
    int err = snd_pcm_prepare(pcm_);
    if (err < 0) {
        running_ = false;
        return Result<void>::err(
            ErrorDomain::Device, ErrorCode::DeviceBusy,
            std::string("snd_pcm_prepare failed: ") + snd_strerror(err));
    }

    thread_ = std::thread(&ALSAAudioCapture::capture_loop, this);
    return Result<void>::ok();
}

void ALSAAudioCapture::stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void ALSAAudioCapture::close() {
    stop();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    is_open_ = false;
}

// ============================================================
// 能力查询
// ============================================================

std::vector<AudioCaptureCapability> ALSAAudioCapture::capabilities() const {
    // 动态枚举 ALSA 设备比较复杂，返回空以表示"需用户指定设备名"
    return {};
}

AudioCaptureConfig ALSAAudioCapture::current_config() const {
    return config_;
}

// ============================================================
// 采集主循环
// ============================================================

void ALSAAudioCapture::capture_loop() {
    int channels = config_.target_channels;
    snd_pcm_uframes_t period_size = static_cast<snd_pcm_uframes_t>(config_.target_frame_size);
    int frame_size_bytes = channels * 2;  // S16_LE: 2 bytes per sample
    int buf_bytes = static_cast<int>(period_size) * frame_size_bytes;

    // 分配读取缓冲
    std::vector<uint8_t> buffer(buf_bytes);
    int64_t frame_idx = 0;

    LOG_I("alsa", "capture loop started: period=%lu frames, buf=%d bytes",
          period_size, buf_bytes);

    while (!stop_requested_) {
        // 阻塞读取一个 period
        int err = snd_pcm_readi(pcm_, buffer.data(), period_size);
        if (err == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (err == -EPIPE) {
            // XRUN (overrun): 恢复并继续
            LOG_W("alsa", "XRUN (overrun), recovering...");
            snd_pcm_recover(pcm_, err, 1);
            continue;
        }
        if (err == -ESTRPIPE) {
            // 挂起（如 suspend/resume）
            while ((err = snd_pcm_resume(pcm_)) == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (err < 0) {
                snd_pcm_prepare(pcm_);
            }
            continue;
        }
        if (err < 0) {
            LOG_E("alsa", "snd_pcm_readi error: %s", snd_strerror(err));
            on_error_(ErrorDomain::Device, ErrorCode::DeviceDisconnected,
                      std::string("snd_pcm_readi: ") + snd_strerror(err));
            break;
        }

        snd_pcm_uframes_t frames_read = static_cast<snd_pcm_uframes_t>(err);
        if (frames_read == 0) continue;

        // 构建 AudioFrame — S16 interleaved, 单 Plane
        int data_bytes = static_cast<int>(frames_read) * frame_size_bytes;
        auto buf = std::make_shared<CpuFrameBuffer>(data_bytes);
        memcpy(buf->data(), buffer.data(), data_bytes);

        AudioFrame af;
        af.format = SampleFormat::S16;
        af.sample_rate = config_.target_sample_rate;
        af.channels = channels;
        af.num_samples = static_cast<int>(frames_read);
        af.duration = TimeDeltaUs::from_samples(af.num_samples, af.sample_rate);
        af.pts.us = frame_idx * af.duration.us;
        af.frame_index = frame_idx;
        af.planes[0].data = buf->data(); af.planes[0].size = static_cast<size_t>(data_bytes); af.planes[0].stride = 0; af.planes[0].offset = 0;
        af.num_planes = 1;
        af.buffer = std::move(buf);

        frame_idx++;
        on_frame_(std::move(af));
    }

    // 停止时排空 ALSA 缓冲
    snd_pcm_drain(pcm_);
    LOG_I("alsa", "capture loop exiting, total frames=%ld", frame_idx);
}

}  // namespace streambridge
