#include "ffmpeg_audio_decoder.h"

#include "streambridge/ffmpeg_utils.h"
#include "streambridge/logging.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace streambridge::android::ffmpeg {

// Common FFmpeg RAII (now in streambridge::)
using streambridge::make_avcodec;
using streambridge::make_avframe;
using streambridge::make_avpacket;

namespace {

static constexpr char kTag[] = "StreamBridgeADec";

}  // namespace

FFmpegAudioDecoder::FFmpegAudioDecoder() = default;

FFmpegAudioDecoder::~FFmpegAudioDecoder() {
    close();
}

streambridge::Result<void> FFmpegAudioDecoder::open(const streambridge::StreamInfo& info) {
    close();

    if (info.codec != streambridge::CodecId::AAC) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecFormatUnsupported,
            "audio codec is not AAC");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecNotFound,
            "AAC decoder not found in FFmpeg");
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

    ctx->sample_rate = info.sample_rate;
    // Manual channel layout setup to avoid AV_CHANNEL_LAYOUT_STEREO compound literal
    // compatibility issue with NDK clang
    ctx->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
    ctx->ch_layout.nb_channels = 2;
    ctx->ch_layout.u.mask = AV_CH_LAYOUT_STEREO;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
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
    dst_sample_rate_ = codec_ctx_->sample_rate;
    dst_channels_ = codec_ctx_->ch_layout.nb_channels;
    frame_index_ = 0;
    pts_fifo_.clear();

    SB_LOG_I(kTag,
            "decoder output: sample_rate=%d channels=%d sample_fmt=%d codec_tb=%d/%d",
            codec_ctx_->sample_rate,
            codec_ctx_->ch_layout.nb_channels,
            codec_ctx_->sample_fmt,
            codec_tb_.num, codec_tb_.den);

    // Initialize resampler (decoder output format -> S16 interleaved)
    AVChannelLayout out_ch_layout;
    memset(&out_ch_layout, 0, sizeof(out_ch_layout));
    out_ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
    out_ch_layout.nb_channels = dst_channels_;
    out_ch_layout.u.mask = (dst_channels_ == 2) ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;

    int ret_swr = swr_alloc_set_opts2(&swr_ctx_,
                                       &out_ch_layout, AV_SAMPLE_FMT_S16, dst_sample_rate_,
                                       &codec_ctx_->ch_layout, codec_ctx_->sample_fmt,
                                       codec_ctx_->sample_rate,
                                       0, nullptr);
    if (ret_swr < 0 || swr_ctx_ == nullptr) {
        close();
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecOpenFailed,
            "swr_alloc_set_opts2 failed");
    }

    ret_swr = swr_init(swr_ctx_);
    if (ret_swr < 0) {
        close();
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecOpenFailed,
            "swr_init failed");
    }

    return streambridge::Result<void>::ok();
}

streambridge::Result<void> FFmpegAudioDecoder::send_packet(
        const streambridge::MediaPacket& packet) {
    if (codec_ctx_ == nullptr) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "audio decoder not opened");
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

    // Push PTS to FIFO queue before sending
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
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            std::string("avcodec_send_packet failed: ") + errbuf);
    }

    return streambridge::Result<void>::ok();
}

streambridge::Result<FFmpegAudioDecoder::DecodeResult>
FFmpegAudioDecoder::receive_frame() {
    if (codec_ctx_ == nullptr) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "audio decoder not opened");
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

    return frame_to_audio_frame(av_frame.get(), output_pts_us);
}

streambridge::Result<FFmpegAudioDecoder::DecodeResult> FFmpegAudioDecoder::decode(
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

streambridge::Result<FFmpegAudioDecoder::DecodeResult> FFmpegAudioDecoder::drain() {
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

void FFmpegAudioDecoder::close() {
    if (swr_ctx_ != nullptr) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    if (codec_ctx_ != nullptr) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    frame_index_ = 0;
    pts_fifo_.clear();
}

streambridge::Result<FFmpegAudioDecoder::DecodeResult>
FFmpegAudioDecoder::frame_to_audio_frame(const AVFrame* av_frame, int64_t pts_us) {
    if (av_frame == nullptr || swr_ctx_ == nullptr) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    if (av_frame->nb_samples <= 0) {
        DecodeResult empty;
        empty.has_frame = false;
        return streambridge::Result<DecodeResult>::ok(std::move(empty));
    }

    const int src_samples = av_frame->nb_samples;
    const int dst_samples = swr_get_out_samples(swr_ctx_, src_samples);
    if (dst_samples <= 0) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            "swr_get_out_samples returned invalid value");
    }

    const int dst_channels = (dst_channels_ > 0) ? dst_channels_ : av_frame->ch_layout.nb_channels;
    const size_t buffer_size = static_cast<size_t>(dst_samples * dst_channels * 2);
    auto buffer = std::make_shared<streambridge::CpuFrameBuffer>(buffer_size);

    uint8_t* out_data = buffer->data();
    // Build input plane array without const_cast UB
    const uint8_t* in_planes[8] = {};
    for (int i = 0; i < av_frame->ch_layout.nb_channels && i < 8; ++i) {
        in_planes[i] = av_frame->data[i];
    }

    int converted = swr_convert(swr_ctx_, &out_data, dst_samples,
                                in_planes, src_samples);
    if (converted < 0) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            "swr_convert failed");
    }

    // Diagnostic: every 50 frames
    if (frame_index_ % 50 == 0) {
        SB_LOG_I(kTag,
                "frame#%lld: src_samples=%d converted=%d ch=%d "
                "bytes[0..3]=%02x%02x%02x%02x pts_us=%lld q_depth=%zu",
                static_cast<long long>(frame_index_),
                src_samples, converted, dst_channels,
                out_data[0], out_data[1], out_data[2], out_data[3],
                static_cast<long long>(pts_us),
                pts_fifo_.size());
    }

    streambridge::AudioFrame frame;
    frame.format = streambridge::SampleFormat::S16;
    frame.sample_rate = dst_sample_rate_;
    frame.channels = dst_channels;
    frame.num_samples = converted;
    frame.num_planes = 1;
    frame.planes[0].data = buffer->data();
    frame.planes[0].size = buffer->size();
    frame.planes[0].stride = dst_channels * 2;
    frame.planes[0].offset = 0;
    frame.buffer = std::move(buffer);
    frame.frame_index = frame_index_++;

    // PTS from FIFO queue (already in microseconds)
    if (pts_us >= 0) {
        frame.pts = streambridge::TimePointUs{pts_us};
    }
    if (av_frame->duration > 0) {
        frame.duration = streambridge::TimeDeltaUs{
            av_rescale_q(av_frame->duration, codec_tb_, {1, 1'000'000})};
    } else {
        frame.duration = streambridge::TimeDeltaUs::from_samples(converted, dst_sample_rate_);
    }

    DecodeResult result;
    result.has_frame = true;
    result.frame = std::move(frame);
    return streambridge::Result<DecodeResult>::ok(std::move(result));
}

}  // namespace streambridge::android::ffmpeg
