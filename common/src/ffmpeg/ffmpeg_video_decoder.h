#pragma once
// FFmpeg H.264 video decoder — implements IVideoDecoder (CpuFrame output)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <cstdint>

#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/pts_fifo.h"

namespace streambridge::ffmpeg {

class FFmpegVideoDecoder : public streambridge::IVideoDecoder {
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder() override;

    // --- IVideoDecoder ---
    Result<void> open(const StreamInfo& info) override;
    void close() override;
    bool is_open() const override { return codec_ctx_ != nullptr; }

    Result<DecodeStatus> send_packet(const MediaPacket& packet) override;
    Result<DecodeOutput> receive_frame(int timeout_ms) override;

    Result<void> present_frame(uint64_t frame_id, int64_t target_time_ns) override;
    Result<void> discard_frame(uint64_t frame_id) override;

    Result<void> drain() override;
    void flush() override;

    DecoderCapability capability() const override;

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVRational codec_tb_{1, 1};
    int dst_width_ = 0;
    int dst_height_ = 0;
    uint64_t next_frame_id_ = 1;
    PtsFifo pts_fifo_;

    // Internal: convert AVFrame to CpuFrameHandle + VideoFrame
    Result<DecodeOutput> avframe_to_output(const AVFrame* av_frame, int64_t pts_us);
};

}  // namespace streambridge::ffmpeg
