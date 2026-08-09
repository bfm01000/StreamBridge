#include "ffmpeg_video_decoder.h"

#include <android/log.h>

#include "ffmpeg_raii.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace streambridge::android::ffmpeg {
namespace {
// (internal helpers)
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

    // 查找 H.264 解码器
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

    // 设置 extradata（SPS/PPS）
    if (!info.codec_extradata.empty()) {
        ctx->extradata = static_cast<uint8_t*>(av_mallocz(info.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (ctx->extradata == nullptr) {
            return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Resource,
                streambridge::ErrorCode::OutOfMemory,
                "failed to allocate extradata");
        }
        std::memcpy(ctx->extradata, info.codec_extradata.data(), info.codec_extradata.size());
        ctx->extradata_size = static_cast<int>(info.codec_extradata.size());
    }

    ctx->width = info.width;
    ctx->height = info.height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // 设置时间基
    ctx->time_base = {info.time_base.num, info.time_base.den};

    // 打开解码器
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
    dst_width_ = info.width;
    dst_height_ = info.height;
    frame_index_ = 0;
    return streambridge::Result<void>::ok();
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult> FFmpegVideoDecoder::decode(
        const streambridge::MediaPacket& packet) {
    if (codec_ctx_ == nullptr) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "video decoder not opened");
    }

    // 构建 AVPacket
    AVPacket* avpkt = av_packet_alloc();
    if (avpkt == nullptr) {
        return streambridge::Result<DecodeResult>::err(
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

    if (packet.has_valid_pts()) {
        avpkt->pts = packet.pts.us;
    } else {
        avpkt->pts = AV_NOPTS_VALUE;
    }
    if (packet.dts.us >= 0) {
        avpkt->dts = packet.dts.us;
    } else {
        avpkt->dts = AV_NOPTS_VALUE;
    }

    // 发送 packet
    int ret = avcodec_send_packet(codec_ctx_, avpkt);
    av_packet_free(&avpkt);

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            std::string("avcodec_send_packet failed: ") + errbuf);
    }

    // 接收帧
    auto av_frame = make_avframe();
    if (av_frame == nullptr) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Resource,
            streambridge::ErrorCode::OutOfMemory,
            "av_frame_alloc failed");
    }

    ret = avcodec_receive_frame(codec_ctx_, av_frame.get());
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

    return frame_to_video_frame(av_frame.get());
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult> FFmpegVideoDecoder::drain() {
    if (codec_ctx_ == nullptr) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    // 发送 null packet 触发冲刷
    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    auto av_frame = make_avframe();
    if (av_frame == nullptr) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    ret = avcodec_receive_frame(codec_ctx_, av_frame.get());
    if (ret < 0) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    return frame_to_video_frame(av_frame.get());
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
}

streambridge::Result<FFmpegVideoDecoder::DecodeResult>
FFmpegVideoDecoder::frame_to_video_frame(const AVFrame* av_frame) {
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

    // 初始化或重建 sws 上下文（仅在尺寸或格式变化时）
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
                "sws_getContext failed for YUV→RGBA");
        }
    }

    // 分配输出 buffer
    const int kBytesPerPixel = 4;
    const int dst_stride = dst_width * kBytesPerPixel;
    const size_t buffer_size = static_cast<size_t>(dst_stride * dst_height);
    auto buffer = std::make_shared<streambridge::CpuFrameBuffer>(buffer_size);

    // 设置目标平面
    uint8_t* dst_planes[4] = {buffer->data(), nullptr, nullptr, nullptr};
    int dst_strides[4] = {dst_stride, 0, 0, 0};

    // 执行缩放和格式转换
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

    // 构建 VideoFrame
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

    // PTS 从 AVFrame 获取
    if (av_frame->pts != AV_NOPTS_VALUE) {
        frame.pts = streambridge::TimePointUs{av_frame->pts};
    }
    if (av_frame->duration > 0) {
        frame.duration = streambridge::TimeDeltaUs{av_frame->duration};
    }

    DecodeResult result;
    result.has_frame = true;
    result.frame = std::move(frame);
    return streambridge::Result<DecodeResult>::ok(std::move(result));
}

}  // namespace streambridge::android::ffmpeg
