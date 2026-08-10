#include "ffmpeg_video_decoder.h"

#include "streambridge/ffmpeg_utils.h"
#include "streambridge/logging.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace streambridge::ffmpeg {

using streambridge::make_avcodec;
using streambridge::make_avframe;

static constexpr char kTag[] = "StreamBridgeVDec";

FFmpegVideoDecoder::FFmpegVideoDecoder() = default;
FFmpegVideoDecoder::~FFmpegVideoDecoder() { close(); }

// ============================================================
// open / close / capability
// ============================================================

Result<void> FFmpegVideoDecoder::open(const StreamInfo& info) {
    close();

    if (info.codec != CodecId::H264) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported, "not H.264");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                          "H.264 decoder not found");

    auto ctx = make_avcodec(codec);
    if (!ctx) return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                        "alloc codec ctx failed");

    if (!info.codec_extradata.empty()) {
        ctx->extradata = static_cast<uint8_t*>(
            av_mallocz(info.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx->extradata)
            return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory, "extradata");
        std::memcpy(ctx->extradata, info.codec_extradata.data(), info.codec_extradata.size());
        ctx->extradata_size = static_cast<int>(info.codec_extradata.size());
    }

    ctx->width = info.width;
    ctx->height = info.height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = {info.time_base.num, info.time_base.den};

    int ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0) {
        char eb[256]{}; av_strerror(ret, eb, sizeof(eb));
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                  std::string("avcodec_open2: ") + eb);
    }

    codec_ctx_ = ctx.release();
    codec_tb_ = codec_ctx_->time_base;
    dst_width_ = info.width;
    dst_height_ = info.height;
    next_frame_id_ = 1;
    pts_fifo_.clear();

    SB_LOG_I(kTag, "opened %dx%d tb=%d/%d", dst_width_, dst_height_,
             codec_tb_.num, codec_tb_.den);
    return Result<void>::ok();
}

void FFmpegVideoDecoder::close() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_); codec_ctx_ = nullptr; }
    pts_fifo_.clear();
}

DecoderCapability FFmpegVideoDecoder::capability() const {
    DecoderCapability c;
    c.hardware = false;
    c.supports_cpu_output = true;
    c.supports_surface_output = false;
    return c;
}

// ============================================================
// send_packet
// ============================================================

Result<DecodeStatus> FFmpegVideoDecoder::send_packet(const MediaPacket& packet) {
    if (!codec_ctx_)
        return Result<DecodeStatus>::err(ErrorDomain::Internal, ErrorCode::InvalidState, "not opened");

    AVPacket* avpkt = av_packet_alloc();
    if (!avpkt) return Result<DecodeStatus>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory, "av_packet_alloc");

    if (!packet.data.empty()) {
        avpkt->data = const_cast<uint8_t*>(packet.data.data());
        avpkt->size = static_cast<int>(packet.data.size());
    }
    if (packet.has_valid_pts()) pts_fifo_.push(packet.pts.us);
    avpkt->pts = AV_NOPTS_VALUE;
    avpkt->dts = AV_NOPTS_VALUE;

    int ret = avcodec_send_packet(codec_ctx_, avpkt);
    av_packet_free(&avpkt);

    if (ret == AVERROR(EAGAIN)) return Result<DecodeStatus>::ok(DecodeStatus::TryAgain);
    if (ret < 0) {
        pts_fifo_.pop_back();
        char eb[256]{}; av_strerror(ret, eb, sizeof(eb));
        return Result<DecodeStatus>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                          std::string("send_packet: ") + eb);
    }
    return Result<DecodeStatus>::ok(DecodeStatus::FrameReady);
}

// ============================================================
// receive_frame
// ============================================================

Result<DecodeOutput> FFmpegVideoDecoder::receive_frame(int /*timeout_ms*/) {
    if (!codec_ctx_)
        return Result<DecodeOutput>::err(ErrorDomain::Internal, ErrorCode::InvalidState, "not opened");

    auto av_frame = make_avframe();
    if (!av_frame)
        return Result<DecodeOutput>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory, "av_frame_alloc");

    int ret = avcodec_receive_frame(codec_ctx_, av_frame.get());
    if (ret == AVERROR(EAGAIN))
        return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::QueueTimeout, "EAGAIN");
    if (ret == AVERROR_EOF)
        return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::PrematureEOF, "EOF");
    if (ret < 0) {
        char eb[256]{}; av_strerror(ret, eb, sizeof(eb));
        return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                          std::string("receive_frame: ") + eb);
    }

    // PTS: best_effort → frame->pts → PtsFifo fallback
    int64_t pts = av_frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = av_frame->pts;
    int64_t pts_us = -1;
    if (pts != AV_NOPTS_VALUE)
        pts_us = av_rescale_q(pts, codec_tb_, {1, 1'000'000});
    if (pts_us < 0) pts_us = pts_fifo_.pop();

    return avframe_to_output(av_frame.get(), pts_us);
}

// ============================================================
// present / discard
// ============================================================

Result<void> FFmpegVideoDecoder::present_frame(uint64_t, int64_t) {
    return Result<void>::err(ErrorDomain::Internal, ErrorCode::NotImplemented,
                              "CPU decoder: present not supported, use Renderer");
}

Result<void> FFmpegVideoDecoder::discard_frame(uint64_t) {
    // shared_ptr in CpuFrameHandle auto-releases when Session drops it
    return Result<void>::ok();
}

// ============================================================
// drain / flush
// ============================================================

Result<void> FFmpegVideoDecoder::drain() {
    if (!codec_ctx_) return Result<void>::ok();
    avcodec_send_packet(codec_ctx_, nullptr);
    return Result<void>::ok();
}

void FFmpegVideoDecoder::flush() {
    if (codec_ctx_) avcodec_flush_buffers(codec_ctx_);
    pts_fifo_.clear();
}

// ============================================================
// Internal: AVFrame → DecodeOutput
// ============================================================

Result<DecodeOutput> FFmpegVideoDecoder::avframe_to_output(
        const AVFrame* av_frame, int64_t pts_us) {
    if (av_frame->width <= 0 || av_frame->height <= 0)
        return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed, "invalid dims");

    int sw = av_frame->width, sh = av_frame->height;
    AVPixelFormat sf = static_cast<AVPixelFormat>(av_frame->format);
    int dw = dst_width_ > 0 ? dst_width_ : sw;
    int dh = dst_height_ > 0 ? dst_height_ : sh;

    if (!sws_ctx_ || !sws_getCachedContext(sws_ctx_, sw, sh, sf, dw, dh,
            AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr)) {
        if (sws_ctx_) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(sw, sh, sf, dw, dh, AV_PIX_FMT_RGBA,
                                   SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_)
            return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                              "sws_getContext failed");
    }

    int stride = dw * 4;
    size_t buf_sz = static_cast<size_t>(stride * dh);
    auto buffer = std::make_shared<CpuFrameBuffer>(buf_sz);

    uint8_t* planes[4] = {buffer->data(), nullptr, nullptr, nullptr};
    int strides[4] = {stride, 0, 0, 0};

    int h = sws_scale(sws_ctx_, av_frame->data, av_frame->linesize, 0, sh, planes, strides);
    if (h <= 0)
        return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed, "sws_scale");

    VideoFrame vf;
    vf.format = PixelFormat::RGBA;
    vf.width = dw;
    vf.height = dh;
    vf.num_planes = 1;
    vf.planes[0] = {buffer->data(), buf_sz, stride, 0};
    vf.buffer = buffer;
    if (pts_us >= 0) vf.pts = TimePointUs{pts_us};

    DecodeOutput out;
    out.frame_id = next_frame_id_++;
    out.pts_us = pts_us;
    out.payload = CpuFrameHandle{std::move(buffer), std::move(vf)};

    if (out.frame_id <= 5)
        SB_LOG_I(kTag, "frame#%llu pts=%lld", (unsigned long long)out.frame_id, (long long)pts_us);

    return Result<DecodeOutput>::ok(std::move(out));
}

}  // namespace streambridge::ffmpeg
