#pragma once

#include <aaudio/AAudio.h>
#include <cstdint>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

// AAudio 音频输出封装：负责打开/关闭音频流、写入 PCM 帧，并按实际播放进度推算音频主时钟。
class NativeAudioOutput {
public:
    NativeAudioOutput();
    ~NativeAudioOutput();

    NativeAudioOutput(const NativeAudioOutput&) = delete;
    NativeAudioOutput& operator=(const NativeAudioOutput&) = delete;

    streambridge::Result<void> open(int sample_rate, int channels);
    void start();  // call before first write (delays requestStart)
    streambridge::Result<void> write(const streambridge::AudioFrame& frame);
    void close();

    int64_t played_frames() const;
    streambridge::TimePointUs played_media_time_us(streambridge::TimePointUs first_audio_pts_us) const;
    bool is_open() const { return stream_ != nullptr; }

private:
    AAudioStream* stream_ = nullptr;
    bool started_ = false;
    int sample_rate_ = 0;
    int channels_ = 0;
    int64_t submitted_frames_ = 0;
};

}  // namespace streambridge::android
