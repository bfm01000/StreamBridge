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

namespace streambridge::android::ffmpeg {

// Common FFmpeg RAII (now in streambridge::)
using streambridge::make_avcodec;
using streambridge::make_avframe;
using streambridge::make_avpacket;

namespace {

static constexpr char kTag[] = "StreamBridgeVDec";

}  // namespace

FFmpegVideoDecoder::FFmpegVideoDecoder() = default;

FFmpegVideoDecoder::~FFmpegVideoDecoder() {
    close();
}

streambridge::Result<void> FFmpegVideoDecoder::open(const streambridge::StreamInfo& info) {
    close();

    if (info.codec != streambridge::CodecId::H264) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecFormatUnsupported,
            "video codec is not H.264");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecNotFound,
            "H.264 decoder not found in FFmpeg");
    }

    auto ctx = make_avcodec(codec);
    if (ctx == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Resource,
            streambridge::ErrorCode::OutOfMemory,
            "avcodec_alloc_context3 failed");
    }

    if (!info.codec_extradata.empty()) {
        ctx->extradata = static_cast<uint8_t*>(av_mallocz(
            info.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (ctx->extradata == nullptr) {
            return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Resource,
                streambridge::ErrorCode::OutOfMemory,
                "failed to allocate extradata");
        }
        std::memcpy(ctx->extradata, info.codec_extradata.data(),
                    info.codec_extradata.size());
        ctx->extradata_size = static_cast<int>(info.codec_extradata.size());
    }

    ctx->width = info.width;
    ctx->height = info.height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = {info.time_base.num, info.time_base.den};

    int ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0) {
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecOpenFailed,
            std::string("avcodec_open2 failed: ") + errbuf);
    }

    codec_ctx_ = ctx.release();
    codec_tb_ = codec_ctx_->time_base;
    dst_width_ = info.width;
    dst_height_ = info.height;
    frame_index_ = 0;
    pts_fifo_.clear();

    SB_LOG_I(kTag,
            "decoder opened: %dx%d codec_tb=%d/%d",
            dst_width_, dst_height_, codec_tb_.num, codec_tb_.den);

    return streambridge::Result<void>::ok();
}

streambridge::Result<void> FFmpegVideoDecoder::send_packet(
        const streambridge::MediaPacket& packet) {
    if (codec_ctx_ == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "video decoder not opened");
    }

    AVPacket* avpkt = av_packet_alloc();
    if (avpkt == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Resource,
            streambridge::ErrorCode::OutOfMemory,
            "av_packet_alloc failed");
    }

    if (!packet.data.empty()) {
        avpkt->data = const_cast<uint8_t*>(packet.data.data());
        avpkt->size = static_cast<int>(packet.data.size());
    } else {
        avpkt->data = nullptr;
        avpkt->size = 0;
    }

    // Push PTS to queue BEFORE sending (FIFO: first packet in = first frame out)
    if (packet.has_valid_pts()) {
        pts_fifo_.push(packet.pts.us);
    }

    avpkt->pts = AV_NOPTS_VALUE;
    avpkt->dts = AV_NOPTS_VALUE;

    int ret = avcodec_send_packet(codec_ctx_, avpkt);
    av_packet_free(&avpkt);

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        // Send failed: undo the PTS push
        pts_fifo_.pop_back();
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            std::string("avcodec_send_packet failed: ") + errbuf);
    }

    return streambridge::Result<void>::ok();
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult>
FFmpegVideoDecoder::receive_frame() {
    if (codec_ctx_ == nullptr) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "video decoder not opened");
    }

    auto av_frame = make_avframe();
    if (av_frame == nullptr) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Resource,
            streambridge::ErrorCode::OutOfMemory,
            "av_frame_alloc failed");
    }

    int ret = avcodec_receive_frame(codec_ctx_, av_frame.get());
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            DecodeResult result;
            result.has_frame = false;
            return streambridge::Result<DecodeResult>::ok(std::move(result));
        }
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            std::string("avcodec_receive_frame failed: ") + errbuf);
    }

    // Pop PTS from FIFO queue
    int64_t output_pts_us = -1;
    output_pts_us = pts_fifo_.pop();

    return frame_to_video_frame(av_frame.get(), output_pts_us);
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult> FFmpegVideoDecoder::decode(
        const streambridge::MediaPacket& packet) {
    auto send_result = send_packet(packet);
    if (send_result.is_err()) {
        return streambridge::Result<DecodeResult>::err(
            send_result.error_domain(),
            send_result.error_code(),
            send_result.error_message());
    }
    return receive_frame();
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult> FFmpegVideoDecoder::drain() {
    if (codec_ctx_ == nullptr) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    return receive_frame();
}

void FFmpegVideoDecoder::close() {
    if (sws_ctx_ != nullptr) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (codec_ctx_ != nullptr) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    frame_index_ = 0;
    pts_fifo_.clear();
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult>
FFmpegVideoDecoder::frame_to_video_frame(const AVFrame* av_frame, int64_t pts_us) {
    if (av_frame == nullptr) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    if (av_frame->width <= 0 || av_frame->height <= 0) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    const int src_width = av_frame->width;
    const int src_height = av_frame->height;
    const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(av_frame->format);
    const int dst_width = (dst_width_ > 0) ? dst_width_ : src_width;
    const int dst_height = (dst_height_ > 0) ? dst_height_ : src_height;

    if (sws_ctx_ == nullptr ||
            sws_getCachedContext(sws_ctx_, src_width, src_height, src_fmt,
                                 dst_width, dst_height, AV_PIX_FMT_RGBA,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr) == nullptr) {
        if (sws_ctx_ != nullptr) {
            sws_freeContext(sws_ctx_);
        }
        sws_ctx_ = sws_getContext(src_width, src_height, src_fmt,
                                  dst_width, dst_height, AV_PIX_FMT_RGBA,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ctx_ == nullptr) {
            return streambridge::Result<DecodeResult>::err(
                streambridge::ErrorDomain::Codec,
                streambridge::ErrorCode::CodecFormatUnsupported,
                "sws_getContext failed for YUV-RGBA");
        }
    }

    const int kBytesPerPixel = 4;
    const int dst_stride = dst_width * kBytesPerPixel;
    const size_t buffer_size = static_cast<size_t>(dst_stride * dst_height);
    auto buffer = std::make_shared<streambridge::CpuFrameBuffer>(buffer_size);

    uint8_t* dst_planes[4] = {buffer->data(), nullptr, nullptr, nullptr};
    int dst_strides[4] = {dst_stride, 0, 0, 0};

    int result_height = sws_scale(sws_ctx_,
                                   av_frame->data, av_frame->linesize,
                                   0, src_height,
                                   dst_planes, dst_strides);
    if (result_height <= 0) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            "sws_scale failed");
    }

    streambridge::VideoFrame frame;
    frame.format = streambridge::PixelFormat::RGBA;
    frame.width = dst_width;
    frame.height = dst_height;
    frame.num_planes = 1;
    frame.planes[0].data = buffer->data();
    frame.planes[0].size = buffer->size();
    frame.planes[0].stride = dst_stride;
    frame.planes[0].offset = 0;
    frame.buffer = std::move(buffer);
    frame.frame_index = frame_index_++;

    if (pts_us >= 0) {
        frame.pts = streambridge::TimePointUs{pts_us};
    }

    if (av_frame->duration > 0) {
        frame.duration = streambridge::TimeDeltaUs{
            av_rescale_q(av_frame->duration, codec_tb_, {1, 1'000'000})};
    }

    if (frame.frame_index < 5) {
        SB_LOG_I(kTag,
                "frame#%lld pts_us=%lld q_depth=%zu",
                static_cast<long long>(frame.frame_index),
                static_cast<long long>(frame.pts.us),
                pts_fifo_.size());
    }

    DecodeResult result;
    result.has_frame = true;
    result.frame = std::move(frame);
    return streambridge::Result<DecodeResult>::ok(std::move(result));
}

}  // namespace streambridge::android::ffmpeg
