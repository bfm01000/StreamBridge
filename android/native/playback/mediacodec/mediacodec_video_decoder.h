#pragma once
// MediaCodec H.264 video decoder — implements IVideoDecoder (DecoderSurface output)
// Zero-copy: GPU renders directly to ANativeWindow Surface

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ffmpeg/codec_config.h"
#include "mediacodec_raii.h"
#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android::mediacodec {

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

private:
    Result<void> configure_with_surface(ANativeWindow* surface);
    Result<void> try_finish_configuration();
    Result<void> recreate(ANativeWindow* new_surface);

    AMediaCodec* codec_ = nullptr;
    ANativeWindow* surface_ = nullptr;
    streambridge::StreamInfo stream_info_;
    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    int width_ = 0;
    int height_ = 0;
    uint64_t next_frame_id_ = 1;
    bool started_ = false;
    bool saw_eos_ = false;
    bool config_complete_ = false;

    // frame_id → MediaCodec output buffer index
    std::unordered_map<uint64_t, int> frame_map_;

    // Delayed configuration
    streambridge::ffmpeg::CodecConfig pending_config_;
    std::vector<uint8_t> pending_packets_;
};

std::unique_ptr<streambridge::IVideoDecoder> create_video_decoder(ANativeWindow* surface);
std::unique_ptr<streambridge::IAudioDecoder> create_audio_decoder();
Result<void> set_decoder_surface(streambridge::IVideoDecoder* decoder, ANativeWindow* surface);

}  // namespace streambridge::android::mediacodec
