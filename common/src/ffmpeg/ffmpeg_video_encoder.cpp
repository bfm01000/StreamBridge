#include "ffmpeg_video_encoder.h"
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
}

namespace streambridge {

FFmpegVideoEncoder::FFmpegVideoEncoder() = default;

FFmpegVideoEncoder::~FFmpegVideoEncoder() {
    close();
}

Result<void> FFmpegVideoEncoder::open(const VideoEncodeConfig& config) {
    config_ = config;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                 "libx264 encoder not found — FFmpeg not built with --enable-libx264");
    }

    codec_ctx_ = alloc_codec_context(codec);
    if (!codec_ctx_) {
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                 "avcodec_alloc_context3 failed");
    }

    AVCodecContext* ctx = codec_ctx_.get();
    ctx->width = config.width;
    ctx->height = config.height;
    ctx->pix_fmt = to_av_pixel_format(config.input_format);
    ctx->time_base = {1, 1'000'000};  // 微秒 time base
    ctx->framerate = {static_cast<int>(config.frame_rate), 1};
    ctx->bit_rate = config.bitrate_bps;
    ctx->gop_size = config.gop_size;
    ctx->max_b_frames = config.b_frames;

    // 低延迟参数
    av_opt_set(ctx->priv_data, "preset", config.preset.c_str(), 0);
    av_opt_set(ctx->priv_data, "tune", config.tune.c_str(), 0);
    if (!config.profile.empty()) {
        av_opt_set(ctx->priv_data, "profile", config.profile.c_str(), 0);
    }

    // 禁用 B 帧的另一种确保方式
    ctx->max_b_frames = 0;
    av_opt_set(ctx->priv_data, "bframes", "0", 0);

    // 设置 GLOBAL_HEADER 让编码器把 SPS/PPS 写入 extradata
    // 而非内嵌在第一个编码帧中 — FLV/RTMP 需要 extradata 用于 sequence header
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        codec_ctx_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                 std::string("avcodec_open2 failed: ") + err);
    }

    // 编码第一帧以触发 SPS/PPS 生成（libx264 在 open 后未必立即有 extradata）
    // 实际方案：设置 AV_CODEC_FLAG_GLOBAL_HEADER 让 libx264 在 open 后生成 extradata
    // 已经 open 了，extradata 应该已经可用；如果为空则在首次 encode 时补充

    packet_ = alloc_packet();
    is_open_ = true;
    frame_count_ = 0;
    return Result<void>::ok();
}

Result<std::vector<MediaPacket>> FFmpegVideoEncoder::encode(VideoFrame frame) {
    if (!is_open_) {
        return Result<std::vector<MediaPacket>>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Encoder not open");
    }

    // 格式/分辨率转换（源帧与编码器配置不一致时自动缩放）
    AVFramePtr avf(videoframe_to_avframe(frame));
    AVFrame* frame_to_send = avf.get();
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(avf->format);
    AVPixelFormat target_fmt = to_av_pixel_format(config_.input_format);
    bool fmt_mismatch = (src_fmt != target_fmt);
    bool size_mismatch = (avf->width != config_.width || avf->height != config_.height);

    if (fmt_mismatch || size_mismatch) {
        if (!sws_ || sws_src_width_ != avf->width || sws_src_height_ != avf->height) {
            sws_.reset(sws_getContext(
                avf->width, avf->height, src_fmt,
                config_.width, config_.height, target_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr));
            sws_src_width_ = avf->width;
            sws_src_height_ = avf->height;
            converted_frame_ = alloc_frame();
            converted_frame_->format = target_fmt;
            converted_frame_->width = config_.width;
            converted_frame_->height = config_.height;
            av_frame_get_buffer(converted_frame_.get(), 0);
        }
        sws_scale(sws_.get(), avf->data, avf->linesize, 0, avf->height,
                  converted_frame_->data, converted_frame_->linesize);
        converted_frame_->pts = avf->pts;
        frame_to_send = converted_frame_.get();
    }

    frame_to_send->pts = frame.pts.us;
    frame_to_send->pict_type = AV_PICTURE_TYPE_NONE;

    int ret = avcodec_send_frame(codec_ctx_.get(), frame_to_send);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return Result<std::vector<MediaPacket>>::err(
            ErrorDomain::Codec, ErrorCode::CodecEncodeFailed,
            std::string("avcodec_send_frame failed: ") + std::to_string(ret));
    }

    std::vector<MediaPacket> packets;
    AVCodecContext* ctx = codec_ctx_.get();
    while (true) {
        av_packet_unref(packet_.get());
        ret = avcodec_receive_packet(ctx, packet_.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            return Result<std::vector<MediaPacket>>::err(
                ErrorDomain::Codec, ErrorCode::CodecEncodeFailed,
                std::string("avcodec_receive_packet failed: ") + std::to_string(ret));
        }

        MediaPacket mp;
        mp.type = MediaType::Video;
        mp.codec = CodecId::H264;
        mp.pts.us = packet_->pts;
        mp.dts.us = packet_->dts;
        mp.is_key_frame = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        mp.data.assign(packet_->data, packet_->data + packet_->size);
        mp.sequence_number = frame_count_;
        packets.push_back(std::move(mp));
    }

    frame_count_++;
    return Result<std::vector<MediaPacket>>::ok(std::move(packets));
}

Result<std::vector<MediaPacket>> FFmpegVideoEncoder::drain() {
    if (!is_open_) {
        return Result<std::vector<MediaPacket>>::ok({});
    }

    int ret = avcodec_send_frame(codec_ctx_.get(), nullptr);
    (void)ret;  // EAGAIN/EOF 都是正常的

    std::vector<MediaPacket> packets;
    AVCodecContext* ctx = codec_ctx_.get();
    while (true) {
        av_packet_unref(packet_.get());
        ret = avcodec_receive_packet(ctx, packet_.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        MediaPacket mp;
        mp.type = MediaType::Video;
        mp.codec = CodecId::H264;
        mp.pts.us = packet_->pts;
        mp.dts.us = packet_->dts;
        mp.is_key_frame = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        mp.data.assign(packet_->data, packet_->data + packet_->size);
        mp.sequence_number = frame_count_;
        packets.push_back(std::move(mp));
    }
    return Result<std::vector<MediaPacket>>::ok(std::move(packets));
}

void FFmpegVideoEncoder::close() {
    sws_.reset();
    sws_src_width_ = 0;
    sws_src_height_ = 0;
    converted_frame_.reset();
    packet_.reset();
    codec_ctx_.reset();
    is_open_ = false;
}

CodecCapability FFmpegVideoEncoder::capability() const {
    CodecCapability cap;
    cap.codec = CodecId::H264;
    cap.is_hardware = false;
    cap.input_pixel_formats = {PixelFormat::YUV420P, PixelFormat::NV12};
    cap.min_bitrate = 100'000;
    cap.max_bitrate = 20'000'000;
    return cap;
}

std::vector<uint8_t> FFmpegVideoEncoder::extradata() const {
    if (!codec_ctx_ || !codec_ctx_->extradata) return {};
    auto* data = codec_ctx_->extradata;
    return std::vector<uint8_t>(data, data + codec_ctx_->extradata_size);
}

}  // namespace streambridge
