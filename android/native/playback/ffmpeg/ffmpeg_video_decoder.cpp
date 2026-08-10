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

// ============================================================
// IVideoDecoder: open / close
// ============================================================

Result<void> FFmpegVideoDecoder::open(const StreamInfo& info) {
    close();

    if (info.codec != CodecId::H264) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                  "video codec is not H.264");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                  "H.264 decoder not found");
    }

    auto ctx = make_avcodec(codec);
    if (ctx == nullptr) {
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                  "avcodec_alloc_context3 failed");
    }

    if (!info.codec_extradata.empty()) {
        ctx->extradata = static_cast<uint8_t*>(
            av_mallocz(info.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (ctx->extradata == nullptr) {
            return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                      "extradata alloc failed");
        }
        std::memcpy(ctx->extradata, info.codec_extradata.data(), info.codec_extradata.size());
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
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                  std::string("avcodec_open2: ") + errbuf);
    }

    codec_ctx_ = ctx.release();
    codec_tb_ = codec_ctx_->time_base;
    dst_width_ = info.width;
    dst_height_ = info.height;
    frame_index_ = 0;
    pts_fifo_.clear();
    cached_frame_valid_ = false;

    SB_LOG_I(kTag, "decoder opened: %dx%d codec_tb=%d/%d",
             dst_width_, dst_height_, codec_tb_.num, codec_tb_.den);

    return Result<void>::ok();
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
    cached_frame_valid_ = false;
}

DecodeCapability FFmpegVideoDecoder::capability() const {
    DecodeCapability cap;
    cap.codec = CodecId::H264;
    cap.is_hardware = false;
    cap.hardware_name = "FFmpeg libavcodec";
    cap.output_pixel_formats = {PixelFormat::RGBA, PixelFormat::YUV420P};
    return cap;
}

// ============================================================
// IVideoDecoder: send_packet / dequeue_output / release_output
// ============================================================

Result<void> FFmpegVideoDecoder::send_packet(const MediaPacket& packet) {
    if (codec_ctx_ == nullptr) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                  "decoder not opened");
    }

    AVPacket* avpkt = av_packet_alloc();
    if (avpkt == nullptr) {
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                  "av_packet_alloc failed");
    }

    if (!packet.data.empty()) {
        avpkt->data = const_cast<uint8_t*>(packet.data.data());
        avpkt->size = static_cast<int>(packet.data.size());
    } else {
        avpkt->data = nullptr;
        avpkt->size = 0;
    }

    if (packet.has_valid_pts()) {
        pts_fifo_.push(packet.pts.us);
    }

    avpkt->pts = AV_NOPTS_VALUE;
    avpkt->dts = AV_NOPTS_VALUE;

    int ret = avcodec_send_packet(codec_ctx_, avpkt);
    av_packet_free(&avpkt);

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        pts_fifo_.pop_back();
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                  std::string("avcodec_send_packet: ") + errbuf);
    }

    return Result<void>::ok();
}

Result<bool> FFmpegVideoDecoder::decode_one_frame() {
    auto av_frame = make_avframe();
    if (av_frame == nullptr) {
        return Result<bool>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                  "av_frame_alloc failed");
    }

    int ret = avcodec_receive_frame(codec_ctx_, av_frame.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return Result<bool>::ok(false);  // no frame available
    }
    if (ret < 0) {
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return Result<bool>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                  std::string("avcodec_receive_frame: ") + errbuf);
    }

    int64_t pts_us = pts_fifo_.pop();

    auto frame_result = avframe_to_video_frame(av_frame.get(), pts_us);
    if (frame_result.is_err()) {
        return Result<bool>::err(frame_result.error_domain(), frame_result.error_code(),
                                  frame_result.error_message());
    }

    cached_frame_ = std::move(*frame_result);
    cached_frame_valid_ = true;
    return Result<bool>::ok(true);
}

Result<DecodeOutputInfo> FFmpegVideoDecoder::dequeue_output(int64_t /*timeout_us*/) {
    // FFmpeg software decode: try to get a frame immediately (non-blocking)
    // The caller should loop with pop timeout at the session level
    auto result = decode_one_frame();
    if (result.is_err()) {
        return Result<DecodeOutputInfo>::err(result.error_domain(), result.error_code(),
                                              result.error_message());
    }

    DecodeOutputInfo info;
    if (*result && cached_frame_valid_) {
        info.has_output = true;
        info.pts_us = cached_frame_.pts.us;
        info.duration_us = cached_frame_.duration.us;
        info.output_index = static_cast<int>(cached_frame_.frame_index);
    }
    return Result<DecodeOutputInfo>::ok(info);
}

void FFmpegVideoDecoder::release_output(int /*output_index*/, bool /*render*/) {
    // CPU mode: frame stays in cache until next dequeue_output overwrites it
    // Nothing to release
}

Result<IVideoDecoder::CpuFrameResult> FFmpegVideoDecoder::receive_frame(int output_index) {
    CpuFrameResult result;
    if (!cached_frame_valid_ || output_index != static_cast<int>(cached_frame_.frame_index)) {
        result.has_frame = false;
        return Result<CpuFrameResult>::ok(std::move(result));
    }

    result.has_frame = true;
    result.frame = std::move(cached_frame_);
    cached_frame_valid_ = false;
    return Result<CpuFrameResult>::ok(std::move(result));
}

// ============================================================
// IVideoDecoder: drain / flush
// ============================================================

Result<void> FFmpegVideoDecoder::drain() {
    if (codec_ctx_ == nullptr) {
        return Result<void>::ok();
    }

    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        return Result<void>::ok();
    }

    // Drain frames into cache — caller should call dequeue_output + receive_frame
    while (true) {
        auto result = decode_one_frame();
        if (result.is_err() || !*result) break;
    }
    return Result<void>::ok();
}

void FFmpegVideoDecoder::flush() {
    if (codec_ctx_ != nullptr) {
        avcodec_flush_buffers(codec_ctx_);
    }
    pts_fifo_.clear();
    cached_frame_valid_ = false;
}

// ============================================================
// Internal: AVFrame -> VideoFrame
// ============================================================

Result<VideoFrame> FFmpegVideoDecoder::avframe_to_video_frame(
        const AVFrame* av_frame, int64_t pts_us) {
    if (av_frame->width <= 0 || av_frame->height <= 0) {
        return Result<VideoFrame>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                        "invalid frame dimensions");
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
        if (sws_ctx_ != nullptr) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(src_width, src_height, src_fmt,
                                  dst_width, dst_height, AV_PIX_FMT_RGBA,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ctx_ == nullptr) {
            return Result<VideoFrame>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                            "sws_getContext failed");
        }
    }

    const int kBytesPerPixel = 4;
    const int dst_stride = dst_width * kBytesPerPixel;
    const size_t buffer_size = static_cast<size_t>(dst_stride * dst_height);
    auto buffer = std::make_shared<CpuFrameBuffer>(buffer_size);

    uint8_t* dst_planes[4] = {buffer->data(), nullptr, nullptr, nullptr};
    int dst_strides[4] = {dst_stride, 0, 0, 0};

    int result_height = sws_scale(sws_ctx_, av_frame->data, av_frame->linesize,
                                   0, src_height, dst_planes, dst_strides);
    if (result_height <= 0) {
        return Result<VideoFrame>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                        "sws_scale failed");
    }

    VideoFrame frame;
    frame.format = PixelFormat::RGBA;
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
        frame.pts = TimePointUs{pts_us};
    }
    if (av_frame->duration > 0) {
        frame.duration = TimeDeltaUs{
            av_rescale_q(av_frame->duration, codec_tb_, {1, 1'000'000})};
    }

    if (frame.frame_index < 5) {
        SB_LOG_I(kTag, "frame#%lld pts_us=%lld q_depth=%zu",
                 static_cast<long long>(frame.frame_index),
                 static_cast<long long>(frame.pts.us),
                 pts_fifo_.size());
    }

    return Result<VideoFrame>::ok(std::move(frame));
}

}  // namespace streambridge::android::ffmpeg
