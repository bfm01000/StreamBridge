#pragma once

#include "streambridge/codec.h"
#include "../ffmpeg_utils.h"

namespace streambridge {

// 基于 FFmpeg 的 AAC 音频编码器，实现 IAudioEncoder 接口
class FFmpegAudioEncoder : public IAudioEncoder {
public:
    FFmpegAudioEncoder();
    ~FFmpegAudioEncoder() override;

    Result<void> open(const AudioEncodeConfig& config) override;
    Result<std::vector<MediaPacket>> encode(AudioFrame frame) override;
    Result<std::vector<MediaPacket>> drain() override;
    void close() override;

    CodecCapability capability() const override;
    std::vector<uint8_t> extradata() const override;

    bool is_open() const override { return is_open_; }

private:
    AVCodecContextPtr codec_ctx_;
    AVPacketPtr packet_;
    SwrContextPtr swr_;
    AVFramePtr converted_frame_;
    AudioEncodeConfig config_;
    bool is_open_ = false;
    int64_t frame_count_ = 0;
    // 缓存 swr 输入参数（SwrContext 不透明，无法读取）
    int swr_in_sample_rate_ = 0;
    AVSampleFormat swr_in_fmt_ = AV_SAMPLE_FMT_NONE;
    // 输入缓冲：累积不足 1024 的采样帧（ALSA period 通常不是 1024 的倍数）
    std::vector<uint8_t> acc_buffer_;
    int acc_samples_ = 0;  // 已累积的每声道采样数
    int acc_channels_ = 0;
    SampleFormat acc_format_ = SampleFormat::Unknown;
};

}  // namespace streambridge
