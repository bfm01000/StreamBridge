#pragma once

#include <aaudio/AAudio.h>
#include <cstdint>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

class NativeAudioOutput {
public:
    NativeAudioOutput();
    ~NativeAudioOutput();

    NativeAudioOutput(const NativeAudioOutput&) = delete;
    NativeAudioOutput& operator=(const NativeAudioOutput&) = delete;

    streambridge::Result<void> open(int sample_rate, int channels);
    streambridge::Result<void> write(const streambridge::AudioFrame& frame);
    void close();

    int64_t played_frames() const;
    streambridge::TimePointUs played_media_time_us(streambridge::TimePointUs first_audio_pts_us) const;
    bool is_open() const { return stream_ != nullptr; }

private:
    AAudioStream* stream_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    int64_t submitted_frames_ = 0;
};

}  // namespace streambridge::android
