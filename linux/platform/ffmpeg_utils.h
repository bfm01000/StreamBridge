#pragma once
// FFmpeg 对象 RAII 封装 + 类型转换

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// RAII 类型别名
// ============================================================
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); }
};
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); }
};
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* p) const { if (p) av_frame_free(&p); }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct AVPacketDeleter {
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFormatWriteDeleter {
    void operator()(AVFormatContext* p) const { if (p) { if (p->pb) avio_closep(&p->pb); avformat_free_context(p); } }
};
using AVFormatWritePtr = std::unique_ptr<AVFormatContext, AVFormatWriteDeleter>;

struct SwsContextDeleter {
    void operator()(SwsContext* p) const { if (p) sws_freeContext(p); }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct SwrContextDeleter {
    void operator()(SwrContext* p) const { if (p) swr_free(&p); }
};
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

// ============================================================
// 便捷创建函数
// ============================================================
inline AVFormatContextPtr alloc_input_format_context() {
    AVFormatContext* ctx = avformat_alloc_context();
    return AVFormatContextPtr(ctx);
}

inline AVCodecContextPtr alloc_codec_context(const AVCodec* codec) {
    return AVCodecContextPtr(avcodec_alloc_context3(codec));
}

inline AVFramePtr alloc_frame() {
    return AVFramePtr(av_frame_alloc());
}

inline AVPacketPtr alloc_packet() {
    return AVPacketPtr(av_packet_alloc());
}

// ============================================================
// YUV420P 布局计算
// ============================================================

struct Yuv420PLayout {
    int y_stride;
    int uv_stride;
    size_t y_size;
    size_t u_size;
    size_t v_size;
    size_t total_size;
};

inline Yuv420PLayout compute_yuv420p_layout(int width, int height) {
    Yuv420PLayout lo;
    lo.y_stride = width;
    lo.uv_stride = (width + 1) / 2;
    int uv_h = (height + 1) / 2;
    lo.y_size = static_cast<size_t>(lo.y_stride) * height;
    lo.u_size = static_cast<size_t>(lo.uv_stride) * uv_h;
    lo.v_size = static_cast<size_t>(lo.uv_stride) * uv_h;
    lo.total_size = lo.y_size + lo.u_size + lo.v_size;
    return lo;
}

// ============================================================
// 转换函数
// ============================================================

// AVRational → Rational
inline Rational from_avrational(AVRational r) { return {r.num, r.den}; }

// 时间基转换
inline int64_t pts_to_us(int64_t pts, AVRational tb) {
    return av_rescale_q(pts, tb, {1, 1'000'000});
}
inline int64_t us_to_pts(int64_t us, AVRational tb) {
    return av_rescale_q(us, {1, 1'000'000}, tb);
}

// AVFrame → VideoFrame（单次 alloc + memcpy）
VideoFrame avframe_to_videoframe(const AVFrame* avf, AVRational tb);

// VideoFrame → AVFrame（用于编码器输入）
AVFrame* videoframe_to_avframe(const VideoFrame& vf);

// AVFrame → AudioFrame（单次 alloc + memcpy）
AudioFrame avframe_to_audioframe(const AVFrame* avf, AVRational tb);

// PixelFormat / SampleFormat 转换
AVPixelFormat to_av_pixel_format(PixelFormat fmt);
PixelFormat from_av_pixel_format(AVPixelFormat fmt);
AVSampleFormat to_av_sample_format(SampleFormat fmt);
SampleFormat from_av_sample_format(AVSampleFormat fmt);

// 错误码转换
ErrorCode ffmpeg_error_to_error_code(int averr);
Result<void> check_ffmpeg(int ret, const char* context);

}  // namespace streambridge
