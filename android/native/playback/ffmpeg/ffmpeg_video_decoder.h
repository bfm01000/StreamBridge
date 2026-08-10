#pragma once
// FFmpeg H.264 video decoder — implements common IVideoDecoder (CpuFrame mode)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/pts_fifo.h"

namespace streambridge::android::ffmpeg {

class FFmpegVideoDecoder : public streambridge::IVideoDecoder {
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder() override;

    FFmpegVideoDecoder(const FFmpegVideoDecoder&) = delete;
    FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;

    // --- IVideoDecoder interface ---
    Result<void> open(const StreamInfo& info) override;
    void close() override;
    bool is_open() const override { return codec_ctx_ != nullptr; }

    Result<void> send_packet(const MediaPacket& packet) override;
    Result<DecodeOutputInfo> dequeue_output(int64_t timeout_us) override;
    void release_output(int output_index, bool render) override;
    Result<void> drain() override;
    void flush() override;

    OutputMode output_mode() const override { return OutputMode::CpuFrame; }
    DecodeCapability capability() const override;

    // CPU mode: retrieve decoded frame for output_index
    Result<CpuFrameResult> receive_frame(int output_index) override;

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVRational codec_tb_{1, 1};
    int dst_width_ = 0;
    int dst_height_ = 0;
    int64_t frame_index_ = 0;
    PtsFifo pts_fifo_;

    // Cached decoded frame (filled by dequeue_output, consumed by receive_frame)
    bool cached_frame_valid_ = false;
    VideoFrame cached_frame_;

    // Internal: decode one frame from avcodec, store in cache
    Result<bool> decode_one_frame();

    // Internal: AVFrame -> VideoFrame conversion + swscale
    Result<VideoFrame> avframe_to_video_frame(const AVFrame* av_frame, int64_t pts_us);
};

}  // namespace streambridge::android::ffmpeg
