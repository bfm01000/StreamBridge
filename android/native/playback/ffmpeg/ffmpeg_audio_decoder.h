#pragma once
// FFmpeg AAC audio decoder — implements common IAudioDecoder

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/pts_fifo.h"

namespace streambridge::android::ffmpeg {

class FFmpegAudioDecoder : public streambridge::IAudioDecoder {
public:
    FFmpegAudioDecoder();
    ~FFmpegAudioDecoder() override;

    FFmpegAudioDecoder(const FFmpegAudioDecoder&) = delete;
    FFmpegAudioDecoder& operator=(const FFmpegAudioDecoder&) = delete;

    // --- IAudioDecoder interface ---
    Result<void> open(const StreamInfo& info) override;
    void close() override;
    bool is_open() const override { return codec_ctx_ != nullptr; }

    Result<void> send_packet(const MediaPacket& packet) override;
    Result<DecodeResult> receive_frame() override;
    Result<void> drain() override;
    void flush() override;

    DecodeCapability capability() const override;

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    AVRational codec_tb_{1, 1};
    int dst_sample_rate_ = 0;
    int dst_channels_ = 0;
    int64_t frame_index_ = 0;
    PtsFifo pts_fifo_;

    // Internal decode + convert
    Result<DecodeResult> decode_one_frame();
};

}  // namespace streambridge::android::ffmpeg
