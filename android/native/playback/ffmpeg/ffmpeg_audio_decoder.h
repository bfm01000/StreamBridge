#pragma once
// FFmpeg AAC 音频解码器
// 接收 MediaPacket，输出 S16 interleaved AudioFrame（供 AAudio 输出）

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

    // 用流信息初始化 AAC 解码器
    streambridge::Result<void> open(const streambridge::StreamInfo& info);

    // 解码一个 packet，可产出 0 个或多个 AudioFrame
    struct DecodeResult {
        bool has_frame = false;
        streambridge::AudioFrame frame;
    };
    streambridge::Result<DecodeResult> decode(const streambridge::MediaPacket& packet);

    // 冲刷解码器
    streambridge::Result<DecodeResult> drain();

    void close();
    bool is_open() const { return codec_ctx_ != nullptr; }

private:
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    int dst_sample_rate_ = 0;
    int dst_channels_ = 0;
    int64_t frame_index_ = 0;

    // AVFrame → AudioFrame 转换（含 swresample FLTP→S16）
    streambridge::Result<DecodeResult> frame_to_audio_frame(const AVFrame* av_frame);
};

}  // namespace streambridge::android::ffmpeg
