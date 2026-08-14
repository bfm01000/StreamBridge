#pragma once
// ALSAAudioOutput — ALSA 播放（第一版 Linux 播放端音频输出）
// 输入 S16 interleaved（FFmpegAudioDecoder 已转换为 S16），阻塞写入设备；
// played_frames() 暴露「实际播放进度」（写入帧数 - 设备缓冲中未播帧数），
// 供 MediaClock 音频主时钟使用（docs/architecture.md §305 要求）。

#include <cstdint>
#include <string>

#include <alsa/asoundlib.h>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

class ALSAAudioOutput {
public:
    ALSAAudioOutput() = default;
    ~ALSAAudioOutput();

    ALSAAudioOutput(const ALSAAudioOutput&) = delete;
    ALSAAudioOutput& operator=(const ALSAAudioOutput&) = delete;

    // device: ALSA 设备名（如 "default" / "hw:0,0"）
    Result<void> open(const std::string& device, int sample_rate, int channels);
    void close();
    bool is_open() const { return pcm_ != nullptr; }

    // 阻塞写入一帧（S16 interleaved）；返回实际写入的采样数（单声道采样）
    // XRUN 自动恢复后继续写入
    Result<int> write(const AudioFrame& frame);

    // 实际已播放的单声道采样帧数（写入 - 设备缓冲剩余）
    int64_t played_frames() const;

    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }

private:
    snd_pcm_t* pcm_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    int64_t total_written_ = 0;  // 累计写入采样数（单声道）
};

}  // namespace streambridge
