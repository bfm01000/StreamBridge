#include "native_audio_output.h"

#include <algorithm>
#include <string>

#include "streambridge/logging.h"

namespace streambridge::android {
namespace {

streambridge::Result<void> audio_error(streambridge::ErrorCode code, const char* message) {
    return streambridge::Result<void>::err(streambridge::ErrorDomain::Device, code, message);
}

}  // namespace

NativeAudioOutput::NativeAudioOutput() = default;

NativeAudioOutput::~NativeAudioOutput() {
    close();
}

streambridge::Result<void> NativeAudioOutput::open(int sample_rate, int channels) {
    close();
    if (sample_rate <= 0 || channels <= 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Config,
                streambridge::ErrorCode::InvalidConfig,
                "invalid audio output config");
    }

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == nullptr) {
        return audio_error(streambridge::ErrorCode::DeviceNotFound, "failed to create AAudio builder");
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, channels);

    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
        stream_ = nullptr;
        return audio_error(streambridge::ErrorCode::DeviceBusy, "failed to open AAudio stream");
    }

    // Don't call requestStart yet — delay until first audio frame is ready
    // This prevents AAudio from playing silence and skewing the clock
    started_ = false;

    sample_rate_ = AAudioStream_getSampleRate(stream_);
    channels_ = AAudioStream_getChannelCount(stream_);
    submitted_frames_ = 0;

    SB_LOG_I("StreamBridgeAOut",
            "AAudio actual: rate=%d ch=%d buf_frames=%d burst=%d",
            sample_rate_, channels_,
            AAudioStream_getBufferSizeInFrames(stream_),
            AAudioStream_getFramesPerBurst(stream_));

    return streambridge::Result<void>::ok();
}

void NativeAudioOutput::start() {
    if (stream_ != nullptr && !started_) {
        AAudioStream_requestStart(stream_);
        started_ = true;
    }
}

streambridge::Result<void> NativeAudioOutput::write(const streambridge::AudioFrame& frame) {
    if (stream_ == nullptr) {
        return audio_error(streambridge::ErrorCode::DeviceNotFound, "AAudio stream is not open");
    }
    if (!frame.is_valid() || frame.format != streambridge::SampleFormat::S16 ||
            frame.num_planes < 1 || frame.planes[0].data == nullptr) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Config,
                streambridge::ErrorCode::InvalidArgument,
                "only interleaved S16 audio frames are supported");
    }

    const int32_t frames_to_write = frame.num_samples;
    const auto* data = static_cast<const void*>(frame.planes[0].data);
    const int64_t timeout_ns = 100'000'000;
    aaudio_result_t result = AAudioStream_write(stream_, data, frames_to_write, timeout_ns);
    if (result < 0) {
        return audio_error(streambridge::ErrorCode::DeviceDisconnected, "AAudio write failed");
    }
    submitted_frames_ += result;
    if (result != frames_to_write) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Timeout,
                streambridge::ErrorCode::QueueTimeout,
                "AAudio partial write");
    }
    return streambridge::Result<void>::ok();
}

void NativeAudioOutput::close() {
    if (stream_ == nullptr) {
        return;
    }
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    sample_rate_ = 0;
    channels_ = 0;
    submitted_frames_ = 0;
}

int64_t NativeAudioOutput::played_frames() const {
    if (stream_ == nullptr) {
        return 0;
    }

    int64_t frame_position = 0;
    int64_t time_nanoseconds = 0;
    const aaudio_result_t result = AAudioStream_getTimestamp(
            stream_, CLOCK_MONOTONIC, &frame_position, &time_nanoseconds);
    if (result == AAUDIO_OK) {
        return std::max<int64_t>(0, frame_position);
    }
    return std::max<int64_t>(0, submitted_frames_ - AAudioStream_getBufferSizeInFrames(stream_));
}

streambridge::TimePointUs NativeAudioOutput::played_media_time_us(
        streambridge::TimePointUs first_audio_pts_us) const {
    if (sample_rate_ <= 0) {
        return first_audio_pts_us;
    }
    return first_audio_pts_us + streambridge::TimeDeltaUs::from_samples(played_frames(), sample_rate_);
}

}  // namespace streambridge::android
