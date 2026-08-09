#pragma once
// FFmpeg AAC audio decoder
// Receives MediaPacket, outputs S16 interleaved AudioFrame (for AAudio output)

#include <deque>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android::ffmpeg {

class FFmpegAudioDecoder {
public:
    FFmpegAudioDecoder();
    ~FFmpegAudioDecoder();

    FFmpegAudioDecoder(const FFmpegAudioDecoder&) = delete;
    FFmpegAudioDecoder& operator=(const FFmpegAudioDecoder&) = delete;

    // Initialize AAC decoder with stream info
    streambridge::Result<void> open(const streambridge::StreamInfo& info);

    // Send one packet to decoder (pushes PTS to FIFO queue)
    streambridge::Result<void> send_packet(const streambridge::MediaPacket& packet);

    // Receive one decoded frame (pops PTS from FIFO queue)
    struct DecodeResult {
        bool has_frame = false;
        streambridge::AudioFrame frame;
    };
    streambridge::Result<DecodeResult> receive_frame();

    // Combined: send + receive (for backward compatibility / simple cases)
    streambridge::Result<DecodeResult> decode(const streambridge::MediaPacket& packet);

    // Flush decoder
    streambridge::Result<DecodeResult> drain();

    void close();
    bool is_open() const { return codec_ctx_ != nullptr; }

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    AVRational codec_tb_{1, 1};
    int dst_sample_rate_ = 0;
    int dst_channels_ = 0;
    int64_t frame_index_ = 0;

    // PTS FIFO queue: push on send, pop on receive
    std::deque<int64_t> pts_queue_;

    // AVFrame -> AudioFrame conversion (via swresample FLTP->S16)
    streambridge::Result<DecodeResult> frame_to_audio_frame(const AVFrame* av_frame,
                                                             int64_t pts_us);
};

}  // namespace streambridge::android::ffmpeg
