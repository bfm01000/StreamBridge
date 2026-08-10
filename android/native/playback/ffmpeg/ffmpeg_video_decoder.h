#pragma once
// FFmpeg H.264 video decoder
// Receives MediaPacket, outputs RGBA VideoFrame (for ANativeWindow rendering)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/pts_fifo.h"

namespace streambridge::android::ffmpeg {

class FFmpegVideoDecoder {
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder();

    FFmpegVideoDecoder(const FFmpegVideoDecoder&) = delete;
    FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;

    // Initialize decoder with stream info
    streambridge::Result<void> open(const streambridge::StreamInfo& info);

    // Send one packet to decoder (pushes PTS to queue, calls avcodec_send_packet)
    // Returns error if send fails. Does NOT receive frames.
    streambridge::Result<void> send_packet(const streambridge::MediaPacket& packet);

    // Receive one decoded frame (calls avcodec_receive_frame, pops PTS from queue)
    struct DecodeResult {
        bool has_frame = false;
        streambridge::VideoFrame frame;
    };
    streambridge::Result<DecodeResult> receive_frame();

    // Combined: send + receive (convenience, same as send_packet + receive_frame)
    streambridge::Result<DecodeResult> decode(const streambridge::MediaPacket& packet);

    // Flush decoder (send null packet, retrieve buffered frames)
    streambridge::Result<DecodeResult> drain();

    void close();
    bool is_open() const { return codec_ctx_ != nullptr; }

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVRational codec_tb_{1, 1};  // cached decoder time_base for PTS conversion
    int dst_width_ = 0;
    int dst_height_ = 0;
    int64_t frame_index_ = 0;

    // PTS FIFO: tracks packet PTS → output frame PTS ordering
    streambridge::PtsFifo pts_fifo_;

    // AVFrame -> VideoFrame conversion (via swscale YUV->RGBA)
    streambridge::Result<DecodeResult> frame_to_video_frame(const AVFrame* av_frame, int64_t pts_us);
};

}  // namespace streambridge::android::ffmpeg
