#pragma once

#include "streambridge/codec.h"
#include "streambridge/ffmpeg_utils.h"

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
    // 累积缓冲中第一个采样的采集时刻（输入帧 PTS 域，单调时钟），
    // 用于给输出 AAC 帧计算真实时间轴 PTS（与视频共享时间域）
    int64_t acc_pts_start_us_ = -1;
    // 当前累积轮次内已输出的帧数——累积器每轮重启时归零。
    // 不能用全局 frame_count_ 算 PTS：轮次重启后会把上一轮的帧数重复叠加。
    int64_t acc_frame_idx_ = 0;
};

}  // namespace streambridge
