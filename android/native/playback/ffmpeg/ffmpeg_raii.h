#pragma once
// FFmpeg 对象 RAII 封装 — unique_ptr + 自定义 deleter
// 避免裸指针泄漏，适配 FFmpeg 7.0 API

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>

namespace streambridge::android::ffmpeg {

// ============================================================
// AVFormatContext — avformat_close_input
// ============================================================
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx);
    }
};
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

// ============================================================
// AVCodecContext — avcodec_free_context
// ============================================================
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) avcodec_free_context(&ctx);
    }
};
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

// ============================================================
// AVFrame — av_frame_free
// ============================================================
struct AVFrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// ============================================================
// AVPacket — av_packet_free
// ============================================================
struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

// ============================================================
// SwsContext — sws_freeContext
// ============================================================
struct SwsContextDeleter {
    void operator()(SwsContext* ctx) const {
        if (ctx) sws_freeContext(ctx);
    }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

// ============================================================
// SwrContext — swr_free
// ============================================================
struct SwrContextDeleter {
    void operator()(SwrContext* ctx) const {
        if (ctx) swr_free(&ctx);
    }
};
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

// ============================================================
// 工厂函数
// ============================================================
inline AVCodecContextPtr make_avcodec(const AVCodec* codec) {
    return AVCodecContextPtr(avcodec_alloc_context3(codec));
}

inline AVFramePtr make_avframe() {
    return AVFramePtr(av_frame_alloc());
}

inline AVPacketPtr make_avpacket() {
    return AVPacketPtr(av_packet_alloc());
}

}  // namespace streambridge::android::ffmpeg
