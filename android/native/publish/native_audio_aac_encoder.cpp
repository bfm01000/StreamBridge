#include "native_audio_aac_encoder.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>

#include <media/NdkMediaFormat.h>

#include "streambridge/av_sync.h"
#include "streambridge/logging.h"
#include "streambridge/media_types.h"

#ifndef AMEDIAFORMAT_KEY_AAC_PROFILE
#define AMEDIAFORMAT_KEY_AAC_PROFILE "aac-profile"
#endif

namespace streambridge::android {

namespace {
constexpr const char* kTag = "NativeAudioAacEncoder";
constexpr const char* kMimeAac = "audio/mp4a-latm";
constexpr int32_t kAacObjectLc = 2;
constexpr int64_t kCodecTimeoutUs = 10'000;
constexpr int32_t kFramesPerRead = 1024;

std::string media_status_text(media_status_t status) {
    std::ostringstream oss;
    oss << status;
    return oss.str();
}

std::vector<uint8_t> copy_buffer(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    return std::vector<uint8_t>(data, data + size);
}
}  // namespace

NativeAudioAacEncoder::NativeAudioAacEncoder(
        Config config,
        ConfigCallback config_callback,
        PacketCallback packet_callback,
        ErrorCallback error_callback)
    : config_(config),
      config_callback_(std::move(config_callback)),
      packet_callback_(std::move(packet_callback)),
      error_callback_(std::move(error_callback)) {}

NativeAudioAacEncoder::~NativeAudioAacEncoder() {
    stop();
}

int NativeAudioAacEncoder::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return 0;
    }
    captured_frames_ = 0;
    encoded_frames_ = 0;
    encoded_bytes_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        codec_config_.clear();
    }
    worker_ = std::thread(&NativeAudioAacEncoder::worker_loop, this);
    SB_LOG_I(kTag, "starting native audio capture sampleRate=%d channels=%d bitrate=%d",
             config_.sample_rate, config_.channels, config_.bitrate_bps);
    return 0;
}

void NativeAudioAacEncoder::stop() {
    running_ = false;
    if (audio_stream_ != nullptr) {
        AAudioStream_requestStop(audio_stream_);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    release_encoder();
    release_audio_stream();
}

std::vector<uint8_t> NativeAudioAacEncoder::codec_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return codec_config_;
}

void NativeAudioAacEncoder::worker_loop() {
    int result = configure_audio_stream();
    if (result != 0) {
        running_ = false;
        report_error("AAudio configure failed: " + std::to_string(result));
        return;
    }
    result = configure_encoder();
    if (result != 0) {
        running_ = false;
        report_error("AAC encoder configure failed: " + std::to_string(result));
        return;
    }

    aaudio_result_t audio_result = AAudioStream_requestStart(audio_stream_);
    if (audio_result != AAUDIO_OK) {
        running_ = false;
        report_error(std::string("AAudio start failed: ")
                     + AAudio_convertResultToText(audio_result));
        return;
    }

    while (running_) {
        if (!feed_encoder_input()) {
            continue;
        }
        drain_encoder_output(false);
    }
    drain_encoder_output(true);
    SB_LOG_I(kTag, "stopped frames=%lld bytes=%lld",
             static_cast<long long>(encoded_frames_),
             static_cast<long long>(encoded_bytes_));
}

int NativeAudioAacEncoder::configure_audio_stream() {
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == nullptr) {
        return static_cast<int>(result);
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(builder, config_.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, config_.channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

    result = AAudioStreamBuilder_openStream(builder, &audio_stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || audio_stream_ == nullptr) {
        return static_cast<int>(result);
    }

    const int actual_rate = AAudioStream_getSampleRate(audio_stream_);
    const int actual_channels = AAudioStream_getChannelCount(audio_stream_);
    const aaudio_format_t actual_format = AAudioStream_getFormat(audio_stream_);
    if (actual_rate != config_.sample_rate
            || actual_channels != config_.channels
            || actual_format != AAUDIO_FORMAT_PCM_I16) {
        SB_LOG_W(kTag, "AAudio actual format rate=%d channels=%d format=%d",
                 actual_rate, actual_channels, actual_format);
        config_.sample_rate = actual_rate;
        config_.channels = actual_channels;
    }
    return 0;
}

int NativeAudioAacEncoder::configure_encoder() {
    AMediaFormat* format = AMediaFormat_new();
    if (format == nullptr) {
        return -1;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, kMimeAac);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, config_.sample_rate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, config_.channels);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, config_.bitrate_bps);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_AAC_PROFILE, kAacObjectLc);

    encoder_ = AMediaCodec_createEncoderByType(kMimeAac);
    if (encoder_ == nullptr) {
        AMediaFormat_delete(format);
        return -2;
    }
    media_status_t status = AMediaCodec_configure(
        encoder_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) {
        return static_cast<int>(status);
    }
    status = AMediaCodec_start(encoder_);
    if (status != AMEDIA_OK) {
        return static_cast<int>(status);
    }
    return 0;
}

void NativeAudioAacEncoder::release_audio_stream() {
    if (audio_stream_ != nullptr) {
        AAudioStream_close(audio_stream_);
        audio_stream_ = nullptr;
    }
}

void NativeAudioAacEncoder::release_encoder() {
    if (encoder_ != nullptr) {
        AMediaCodec_stop(encoder_);
        AMediaCodec_delete(encoder_);
        encoder_ = nullptr;
    }
}

bool NativeAudioAacEncoder::feed_encoder_input() {
    if (encoder_ == nullptr || audio_stream_ == nullptr) {
        return false;
    }
    ssize_t input_index = AMediaCodec_dequeueInputBuffer(encoder_, kCodecTimeoutUs);
    if (input_index < 0) {
        return false;
    }

    size_t capacity = 0;
    uint8_t* input = AMediaCodec_getInputBuffer(encoder_,
                                                static_cast<size_t>(input_index),
                                                &capacity);
    if (input == nullptr || capacity == 0) {
        return false;
    }

    const int bytes_per_frame = config_.channels * 2;
    const int32_t max_frames =
        static_cast<int32_t>(std::min<size_t>(kFramesPerRead, capacity / bytes_per_frame));
    if (max_frames <= 0) {
        return false;
    }

    aaudio_result_t frames_read = AAudioStream_read(
        audio_stream_, input, max_frames, 20'000'000);
    if (frames_read < 0) {
        if (running_) {
            report_error(std::string("AAudio read failed: ")
                         + AAudio_convertResultToText(frames_read));
        }
        return false;
    }
    if (frames_read == 0) {
        return false;
    }

    const int32_t read_frames = static_cast<int32_t>(frames_read);
    const size_t read_bytes = static_cast<size_t>(read_frames * bytes_per_frame);
    const int64_t pts_us = capture_pts_us(read_frames);
    media_status_t status = AMediaCodec_queueInputBuffer(
        encoder_,
        static_cast<size_t>(input_index),
        0,
        read_bytes,
        pts_us,
        0);
    if (status != AMEDIA_OK && running_) {
        report_error("AAC queueInput failed: " + media_status_text(status));
    }
    return status == AMEDIA_OK;
}

bool NativeAudioAacEncoder::drain_encoder_output(bool final_drain) {
    if (encoder_ == nullptr) {
        return false;
    }
    bool drained = false;
    while (true) {
        AMediaCodecBufferInfo info{};
        ssize_t output_index = AMediaCodec_dequeueOutputBuffer(
            encoder_, &info, final_drain ? kCodecTimeoutUs : 0);
        if (output_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            return drained;
        }
        if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* output_format = AMediaCodec_getOutputFormat(encoder_);
            if (output_format != nullptr) {
                void* csd = nullptr;
                size_t csd_size = 0;
                if (AMediaFormat_getBuffer(output_format, "csd-0", &csd, &csd_size)) {
                    publish_codec_config(copy_buffer(static_cast<const uint8_t*>(csd), csd_size));
                    SB_LOG_I(kTag, "AAC output format csd=%zu rate=%d channels=%d",
                             csd_size, config_.sample_rate, config_.channels);
                }
                AMediaFormat_delete(output_format);
            }
            continue;
        }
        if (output_index < 0) {
            continue;
        }

        size_t buffer_size = 0;
        uint8_t* output = AMediaCodec_getOutputBuffer(
            encoder_, static_cast<size_t>(output_index), &buffer_size);
        if (output != nullptr
                && info.size > 0
                && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
            NativeAudioAacEncoder::EncodedPacket packet;
            packet.data.assign(output + info.offset, output + info.offset + info.size);
            packet.pts_us = info.presentationTimeUs;
            packet.duration_us =
                streambridge::TimeDeltaUs::from_samples(kFramesPerRead, config_.sample_rate).us;
            if (packet_callback_) {
                packet_callback_(std::move(packet));
            }
            encoded_frames_++;
            encoded_bytes_ += info.size;
            drained = true;
        }
        AMediaCodec_releaseOutputBuffer(encoder_, static_cast<size_t>(output_index), false);
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
            return true;
        }
    }
}

int64_t NativeAudioAacEncoder::capture_pts_us(int32_t frames_read) {
    const int64_t buffer_end_frame = captured_frames_ + frames_read;
    int64_t timestamp_frame_position = 0;
    int64_t timestamp_nanos = 0;
    aaudio_result_t result = AAudioStream_getTimestamp(
        audio_stream_,
        CLOCK_MONOTONIC,
        &timestamp_frame_position,
        &timestamp_nanos);

    int64_t pts_us = 0;
    if (result == AAUDIO_OK) {
        const int64_t frame_delta = buffer_end_frame - timestamp_frame_position;
        pts_us = timestamp_nanos / 1000
                 + streambridge::TimeDeltaUs::from_samples(frame_delta, config_.sample_rate).us
                 - streambridge::TimeDeltaUs::from_samples(frames_read, config_.sample_rate).us;
    } else {
        pts_us = streambridge::monotonic_now_us().us
                 - streambridge::TimeDeltaUs::from_samples(frames_read, config_.sample_rate).us;
    }
    captured_frames_ = buffer_end_frame;
    return std::max<int64_t>(0, pts_us);
}

void NativeAudioAacEncoder::publish_codec_config(std::vector<uint8_t> config) {
    if (config.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        codec_config_ = config;
    }
    if (config_callback_) {
        config_callback_(std::move(config));
    }
}

void NativeAudioAacEncoder::report_error(const std::string& message) {
    SB_LOG_E(kTag, "%s", message.c_str());
    if (error_callback_) {
        error_callback_(message);
    }
}

}  // namespace streambridge::android
