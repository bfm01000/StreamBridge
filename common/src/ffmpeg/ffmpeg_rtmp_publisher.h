#pragma once

#include "streambridge/stop_token.h"
#include "streambridge/transport.h"
#include "streambridge/ffmpeg_utils.h"

namespace streambridge {

// 基于 FFmpeg 的 RTMP 推流发布器，实现 IMediaPublisher 接口
class FFmpegRTMPPublisher : public IMediaPublisher {
public:
    FFmpegRTMPPublisher();
    ~FFmpegRTMPPublisher() override;

    Result<void> open(const PublishConfig& config) override;
    Result<void> write_header(const StreamInfo& video_stream,
                              const StreamInfo& audio_stream) override;
    Result<void> write_packet(const MediaPacket& packet) override;
    void close() override;
    void interrupt() override;

    bool is_open() const override { return is_open_; }

    Stats stats() const override {
        return {bytes_written_.load(), packets_written_.load()};
    }

private:
    void setup_stream(const StreamInfo& info, AVStream* stream);

    PublishConfig config_;
    AVFormatWritePtr fmt_ctx_;
    AVIOContext* raw_pb_ = nullptr;
    StopSource stop_source_;
    StopToken stop_token_;  // 保持 StopToken 存活，避免 interrupt callback 的悬垂指针
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> is_open_{false};
    std::atomic<int64_t> bytes_written_{0};
    std::atomic<int64_t> packets_written_{0};

    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    StreamInfo raw_video_stream_;
    TimePointUs stream_start_us_{0};
    bool header_written_ = false;
    bool raw_flv_mode_ = false;
};

}  // namespace streambridge
