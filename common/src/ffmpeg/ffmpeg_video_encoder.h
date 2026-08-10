#pragma once

#include "streambridge/codec.h"
#include "../ffmpeg_utils.h"

namespace streambridge {

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
    AVFramePtr converted_frame_;
    VideoEncodeConfig config_;
    bool is_open_ = false;
    int64_t frame_count_ = 0;
};

}  // namespace streambridge
