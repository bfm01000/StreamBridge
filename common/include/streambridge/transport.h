#pragma once
// 传输接口（RTMP 发布/订阅）

#include <utility>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"
#include "streambridge/stop_token.h"

namespace streambridge {

// 推流配置：RTMP 地址、封装格式与连接/写入超时
struct PublishConfig {
    std::string url;                  // rtmp://127.0.0.1:1935/live/stream0
    std::string format = "flv";
    int connect_timeout_ms = 10'000;
    int write_timeout_ms = 5'000;
};

// 拉流配置：RTMP 地址、超时与缓冲时长
struct SubscribeConfig {
    std::string url;
    int connect_timeout_ms = 10'000;
    int read_timeout_ms = 5'000;
    int64_t buffer_duration_us = 2'000'000;
};

// 推流传输抽象：写 FLV 头与媒体包，支持中断与统计
class IMediaPublisher {
public:
    virtual ~IMediaPublisher() = default;

    virtual Result<void> open(const PublishConfig& config) = 0;
    virtual Result<void> write_header(const StreamInfo& video_stream,
                                      const StreamInfo& audio_stream) = 0;
    virtual Result<void> write_packet(const MediaPacket& packet) = 0;
    virtual void close() = 0;
    virtual void interrupt() = 0;

    virtual bool is_open() const = 0;

    struct Stats {
        int64_t bytes_written = 0;
        int64_t packets_written = 0;
    };
    virtual Stats stats() const = 0;
};

// 拉流传输抽象：读流头与媒体包，支持中断与统计
class IMediaSubscriber {
public:
    virtual ~IMediaSubscriber() = default;

    virtual Result<void> open(const SubscribeConfig& config) = 0;
    virtual Result<std::pair<StreamInfo, StreamInfo>>
        read_header(StopToken stop) = 0;
    virtual Result<MediaPacket> read_packet(StopToken stop) = 0;
    virtual void close() = 0;
    virtual void interrupt() = 0;

    virtual bool is_open() const = 0;

    struct Stats {
        int64_t bytes_read = 0;
        int64_t packets_read = 0;
    };
    virtual Stats stats() const = 0;
};

}  // namespace streambridge
