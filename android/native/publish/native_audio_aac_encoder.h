#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>

namespace streambridge::android {

class NativeAudioAacEncoder {
public:
    struct Config {
        int sample_rate = 48'000;
        int channels = 1;
        int bitrate_bps = 96'000;
    };

    struct EncodedPacket {
        std::vector<uint8_t> data;
        int64_t pts_us = 0;
        int64_t duration_us = 0;
    };

    using ConfigCallback = std::function<void(std::vector<uint8_t>)>;
    using PacketCallback = std::function<void(EncodedPacket)>;
    using ErrorCallback = std::function<void(std::string)>;

    NativeAudioAacEncoder(Config config,
                          ConfigCallback config_callback,
                          PacketCallback packet_callback,
                          ErrorCallback error_callback);
    ~NativeAudioAacEncoder();

    int start();
    void stop();

    Config config() const { return config_; }
    std::vector<uint8_t> codec_config() const;

private:
    void worker_loop();
    int configure_audio_stream();
    int configure_encoder();
    void release_audio_stream();
    void release_encoder();
    bool feed_encoder_input();
    bool drain_encoder_output(bool final_drain);
    int64_t capture_pts_us(int32_t frames_read);
    void publish_codec_config(std::vector<uint8_t> config);
    void report_error(const std::string& message);

    Config config_;
    ConfigCallback config_callback_;
    PacketCallback packet_callback_;
    ErrorCallback error_callback_;

    mutable std::mutex mutex_;
    std::vector<uint8_t> codec_config_;
    AAudioStream* audio_stream_ = nullptr;
    AMediaCodec* encoder_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    int64_t captured_frames_ = 0;
    int64_t encoded_frames_ = 0;
    int64_t encoded_bytes_ = 0;
};

}  // namespace streambridge::android
