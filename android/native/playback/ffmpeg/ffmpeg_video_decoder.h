#pragma once
// FFmpeg H.264 视频解码器
// 接收 MediaPacket，输出 RGBA VideoFrame（供 ANativeWindow 渲染）

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android::ffmpeg {

class FFmpegVideoDecoder {
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder();

    FFmpegVideoDecoder(const FFmpegVideoDecoder&) = delete;
    FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;

    // 用流信息初始化解码器
    streambridge::Result<void> open(const streambridge::StreamInfo& info);

    // 解码一个 packet，可产出 0 个或多个 VideoFrame
    // frame 仅在返回 Ok 且 has_frame 为 true 时有效
    // 调用方应循环调用直到 receive_frame 返回空
    struct DecodeResult {
        bool has_frame = false;
        streambridge::VideoFrame frame;
    };
    streambridge::Result<DecodeResult> decode(const streambridge::MediaPacket& packet);

    // 冲刷解码器（发送 null packet，取回缓冲帧）
    streambridge::Result<DecodeResult> drain();

    void close();
    bool is_open() const { return codec_ctx_ != nullptr; }

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int dst_width_ = 0;
    int dst_height_ = 0;
    int64_t frame_index_ = 0;

    // AVFrame → VideoFrame 转换（含 swscale YUV→RGBA）
    streambridge::Result<DecodeResult> frame_to_video_frame(const AVFrame* av_frame);
};

}  // namespace streambridge::android::ffmpeg
