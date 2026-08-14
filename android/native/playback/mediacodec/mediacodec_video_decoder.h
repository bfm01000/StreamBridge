#pragma once
// MediaCodec H.264 video decoder — implements IVideoDecoder.
// Supports two hardware output paths:
// 1. MediaCodec -> AImageReader -> AHardwareBuffer -> renderer
// 2. MediaCodec -> ANativeWindow Surface

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "ffmpeg/codec_config.h"
#include "hardware_buffer_frame.h"
#include "mediacodec_raii.h"
#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "video_path_config.h"

namespace streambridge::android::mediacodec {

struct MediaCodecReleaseState;

// MediaCodec H.264 视频解码器：实现 IVideoDecoder，解码输出直接渲染到 Surface，支持重配与刷新。
class MediaCodecVideoDecoder : public streambridge::IVideoDecoder {
public:
    MediaCodecVideoDecoder();
    ~MediaCodecVideoDecoder() override;

    // --- IVideoDecoder v2.1 ---
    Result<void> open(const StreamInfo& info) override;
    void close() override;
    bool is_open() const override { return codec_ != nullptr; }

    Result<DecodeStatus> send_packet(const MediaPacket& packet) override;
    Result<DecodeOutput> receive_frame(int timeout_ms) override;

    Result<void> present_frame(uint64_t frame_id, int64_t target_time_ns) override;
    Result<void> discard_frame(uint64_t frame_id) override;

    Result<void> drain() override;
    void flush() override;

    DecoderCapability capability() const override;

    // Surface management (Android-specific, not in common interface)
    Result<void> set_surface(ANativeWindow* window);
    void set_preferred_output_path(streambridge::android::VideoDecodePath path);

private:
    enum class OutputMode {
        Surface,
        HardwareBuffer,
    };

    Result<void> configure_with_surface(ANativeWindow* surface);
    Result<void> try_finish_configuration();
    Result<void> recreate(ANativeWindow* new_surface);
    Result<void> create_image_reader();
    void close_image_reader();
    Result<AImage*> acquire_next_image();
    Result<streambridge::VideoFrame> image_to_video_frame(
            AImage* image,
            AHardwareBuffer* hardware_buffer,
            int64_t pts_us);

    AMediaCodec* codec_ = nullptr;
    std::shared_ptr<MediaCodecReleaseState> release_state_;
    ANativeWindow* surface_ = nullptr;
    AImageReader* image_reader_ = nullptr;
    ANativeWindow* image_reader_window_ = nullptr;
    streambridge::StreamInfo stream_info_;
    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    int width_ = 0;
    int height_ = 0;
    uint64_t next_frame_id_ = 1;
    bool started_ = false;
    bool saw_eos_ = false;
    bool config_complete_ = false;
    OutputMode output_mode_ = OutputMode::HardwareBuffer;
    streambridge::android::VideoDecodePath preferred_path_ =
        streambridge::android::VideoDecodePath::Auto;
    bool image_reader_cpu_readable_ = false;
    int64_t image_acquire_count_ = 0;

    // Delayed configuration
    streambridge::ffmpeg::CodecConfig pending_config_;
    std::vector<uint8_t> pending_packets_;
};

std::unique_ptr<streambridge::IVideoDecoder> create_video_decoder(
    ANativeWindow* surface,
    streambridge::android::VideoDecodePath path);
std::unique_ptr<streambridge::IAudioDecoder> create_audio_decoder();
Result<void> set_decoder_surface(streambridge::IVideoDecoder* decoder, ANativeWindow* surface);

}  // namespace streambridge::android::mediacodec
