#pragma once

#include "streambridge/codec.h"
#include "../ffmpeg_utils.h"

namespace streambridge {

// 基于 FFmpeg 的 H.264 视频编码器，实现 IVideoEncoder 接口
class FFmpegVideoEncoder : public IVideoEncoder {
public:
    FFmpegVideoEncoder();
    ~FFmpegVideoEncoder() override;

    Result<void> open(const VideoEncodeConfig& config) override;
    Result<std::vector<MediaPacket>> encode(VideoFrame frame) override;
    Result<std::vector<MediaPacket>> drain() override;
    void close() override;

    CodecCapability capability() const override;
    std::vector<uint8_t> extradata() const override;

    bool is_open() const override { return is_open_; }

private:
    void cleanup();

    AVCodecContextPtr codec_ctx_;
    AVPacketPtr packet_;
    SwsContextPtr sws_;
    int sws_src_width_ = 0;
    int sws_src_height_ = 0;
    AVFramePtr converted_frame_;
    VideoEncodeConfig config_;
    bool is_open_ = false;
    int64_t frame_count_ = 0;
};

}  // namespace streambridge
