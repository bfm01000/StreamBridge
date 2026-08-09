#pragma once
// FFmpeg RTMP 拉流与 FLV 解封装
// 打开 RTMP URL，读取音视频 packet，转换为公共 MediaPacket

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android::ffmpeg {

class FFmpegSubscriber {
public:
    FFmpegSubscriber();
    ~FFmpegSubscriber();

    FFmpegSubscriber(const FFmpegSubscriber&) = delete;
    FFmpegSubscriber& operator=(const FFmpegSubscriber&) = delete;

    // 打开 RTMP URL 并解析流信息
    // 成功返回 Ok，失败返回 Error（NetworkConnectFailed / InvalidUrl 等）
    streambridge::Result<void> open(const std::string& url);

    // 读取一个 packet，阻塞直到有数据或 EOF/错误
    // 返回 Ok(nullptr) 表示 EOF
    // 返回 Ok(MediaPacket) 表示成功读取
    // 返回 Err 表示读取错误
    streambridge::Result<streambridge::MediaPacket> read_packet();

    // 查询流信息
    const streambridge::StreamInfo* video_stream() const { return has_video_ ? &video_info_ : nullptr; }
    const streambridge::StreamInfo* audio_stream() const { return has_audio_ ? &audio_info_ : nullptr; }
    bool has_video() const { return has_video_; }
    bool has_audio() const { return has_audio_; }

    // 关闭连接，清理资源
    void close();

    // 是否已打开
    bool is_open() const { return fmt_ctx_ != nullptr; }

private:
    // FFmpeg 内部对象由 close() 统一管理
    AVFormatContext* fmt_ctx_ = nullptr;  // 裸指针，由 avformat_close_input 释放

    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    AVRational video_time_base_{1, 1};
    AVRational audio_time_base_{1, 1};

    bool has_video_ = false;
    bool has_audio_ = false;
    streambridge::StreamInfo video_info_;
    streambridge::StreamInfo audio_info_;

    int64_t packet_seq_ = 0;

    // 内部方法：从 FFmpeg codec_id 映射到公共 CodecId
    static streambridge::CodecId map_codec_id(AVCodecID id);
    // 从 FFmpeg sample_fmt 映射到公共 SampleFormat
    static streambridge::SampleFormat map_sample_format(AVSampleFormat fmt);
    // 填充 codec extradata（H.264 SPS/PPS 或 AAC AudioSpecificConfig）
    static void fill_extradata(AVCodecParameters* par, streambridge::StreamInfo& info);
};

}  // namespace streambridge::android::ffmpeg
