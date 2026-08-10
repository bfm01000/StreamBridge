#pragma once
// FFmpeg object RAII wrappers + type conversion helpers
// Shared by Linux and Android — single source of truth

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// RAII deleters + unique_ptr aliases
// ============================================================

struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const {
        if (p) avformat_close_input(&p);
    }
};
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const {
        if (p) avcodec_free_context(&p);
    }
};
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* p) const {
        if (p) av_frame_free(&p);
    }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct AVPacketDeleter {
    void operator()(AVPacket* p) const {
        if (p) av_packet_free(&p);
    }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct SwsContextDeleter {
    void operator()(SwsContext* p) const {
        if (p) sws_freeContext(p);
    }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct SwrContextDeleter {
    void operator()(SwrContext* p) const {
        if (p) swr_free(&p);
    }
};
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

// ============================================================
// Factory functions
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

// ============================================================
// Time base conversion
// ============================================================

inline int64_t pts_to_us(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE) return -1;
    return av_rescale_q(pts, tb, {1, 1'000'000});
}

inline int64_t us_to_pts(int64_t us, AVRational tb) {
    if (us < 0) return AV_NOPTS_VALUE;
    return av_rescale_q(us, {1, 1'000'000}, tb);
}

// ============================================================
// PixelFormat / SampleFormat conversion
// ============================================================

inline AVPixelFormat to_av_pixel_format(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::YUV420P: return AV_PIX_FMT_YUV420P;
        case PixelFormat::NV12:    return AV_PIX_FMT_NV12;
        case PixelFormat::NV21:    return AV_PIX_FMT_NV21;
        case PixelFormat::YUV422P: return AV_PIX_FMT_YUV422P;
        case PixelFormat::YUYV422: return AV_PIX_FMT_YUYV422;
        case PixelFormat::BGRA:    return AV_PIX_FMT_BGRA;
        case PixelFormat::RGBA:    return AV_PIX_FMT_RGBA;
        case PixelFormat::RGB24:   return AV_PIX_FMT_RGB24;
        case PixelFormat::GRAY8:   return AV_PIX_FMT_GRAY8;
        default: return AV_PIX_FMT_NONE;
    }
}

inline PixelFormat from_av_pixel_format(AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_YUV420P: return PixelFormat::YUV420P;
        case AV_PIX_FMT_NV12:    return PixelFormat::NV12;
        case AV_PIX_FMT_NV21:    return PixelFormat::NV21;
        case AV_PIX_FMT_YUV422P: return PixelFormat::YUV422P;
        case AV_PIX_FMT_YUYV422: return PixelFormat::YUYV422;
        case AV_PIX_FMT_BGRA:    return PixelFormat::BGRA;
        case AV_PIX_FMT_RGBA:    return PixelFormat::RGBA;
        case AV_PIX_FMT_RGB24:   return PixelFormat::RGB24;
        case AV_PIX_FMT_GRAY8:   return PixelFormat::GRAY8;
        default: return PixelFormat::Unknown;
    }
}

inline AVSampleFormat to_av_sample_format(SampleFormat fmt) {
    switch (fmt) {
        case SampleFormat::S16:       return AV_SAMPLE_FMT_S16;
        case SampleFormat::S16Planar: return AV_SAMPLE_FMT_S16P;
        case SampleFormat::FLT:       return AV_SAMPLE_FMT_FLT;
        case SampleFormat::FLTPlanar: return AV_SAMPLE_FMT_FLTP;
        default: return AV_SAMPLE_FMT_NONE;
    }
}

inline SampleFormat from_av_sample_format(AVSampleFormat fmt) {
    switch (fmt) {
        case AV_SAMPLE_FMT_S16:  return SampleFormat::S16;
        case AV_SAMPLE_FMT_S16P: return SampleFormat::S16Planar;
        case AV_SAMPLE_FMT_FLT:  return SampleFormat::FLT;
        case AV_SAMPLE_FMT_FLTP: return SampleFormat::FLTPlanar;
        default: return SampleFormat::Unknown;
    }
}

// ============================================================
// Frame conversion (implemented in common/src/ffmpeg_utils.cpp)
// ============================================================

VideoFrame avframe_to_videoframe(const AVFrame* avf, AVRational tb);
AVFrame*  videoframe_to_avframe(const VideoFrame& vf);
AudioFrame avframe_to_audioframe(const AVFrame* avf, AVRational tb);

// ============================================================
// Error helpers
// ============================================================

ErrorCode ffmpeg_error_to_error_code(int averr);
Result<void> check_ffmpeg(int ret, const char* context);

}  // namespace streambridge
