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

// ============================================================
// IAudioDecoder: open / close / capability
// ============================================================

Result<void> FFmpegAudioDecoder::open(const StreamInfo& info) {
    close();

    if (info.codec != CodecId::AAC) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                  "audio codec is not AAC");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                  "AAC decoder not found");
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

    ctx->sample_rate = info.sample_rate;
    ctx->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
    ctx->ch_layout.nb_channels = 2;
    ctx->ch_layout.u.mask = AV_CH_LAYOUT_STEREO;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
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
    dst_sample_rate_ = codec_ctx_->sample_rate;
    dst_channels_ = codec_ctx_->ch_layout.nb_channels;
    frame_index_ = 0;
    pts_fifo_.clear();

    SB_LOG_I(kTag, "decoder: rate=%d ch=%d fmt=%d tb=%d/%d",
             codec_ctx_->sample_rate, codec_ctx_->ch_layout.nb_channels,
             codec_ctx_->sample_fmt, codec_tb_.num, codec_tb_.den);

    // Init swresample: FLTP -> S16 interleaved
    AVChannelLayout out_ch_layout{};
    out_ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
    out_ch_layout.nb_channels = dst_channels_;
    out_ch_layout.u.mask = (dst_channels_ == 2) ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;

    int ret_swr = swr_alloc_set_opts2(&swr_ctx_,
        &out_ch_layout, AV_SAMPLE_FMT_S16, dst_sample_rate_,
        &codec_ctx_->ch_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate, 0, nullptr);
    if (ret_swr < 0 || swr_ctx_ == nullptr) {
        close();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                  "swr_alloc_set_opts2 failed");
    }

    ret_swr = swr_init(swr_ctx_);
    if (ret_swr < 0) {
        close();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                  "swr_init failed");
    }

    return Result<void>::ok();
}

void FFmpegAudioDecoder::close() {
    if (swr_ctx_ != nullptr) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
    if (codec_ctx_ != nullptr) { avcodec_free_context(&codec_ctx_); codec_ctx_ = nullptr; }
    frame_index_ = 0;
    pts_fifo_.clear();
}

DecodeCapability FFmpegAudioDecoder::capability() const {
    DecodeCapability cap;
    cap.codec = CodecId::AAC;
    cap.is_hardware = false;
    cap.hardware_name = "FFmpeg libavcodec";
    cap.output_sample_formats = {SampleFormat::S16};
    return cap;
}

// ============================================================
// IAudioDecoder: send_packet / receive_frame
// ============================================================

Result<void> FFmpegAudioDecoder::send_packet(const MediaPacket& packet) {
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

Result<IAudioDecoder::DecodeResult> FFmpegAudioDecoder::decode_one_frame() {
    IAudioDecoder::DecodeResult result;
    result.has_frame = false;

    auto av_frame = make_avframe();
    if (av_frame == nullptr) {
        return Result<DecodeResult>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                          "av_frame_alloc failed");
    }

    int ret = avcodec_receive_frame(codec_ctx_, av_frame.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return Result<DecodeResult>::ok(std::move(result));
    }
    if (ret < 0) {
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return Result<DecodeResult>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                          std::string("avcodec_receive_frame: ") + errbuf);
    }

    if (av_frame->nb_samples <= 0) {
        return Result<DecodeResult>::ok(std::move(result));
    }

    // PTS
    int64_t pts_us = pts_fifo_.pop();

    // Swresample
    const int src_samples = av_frame->nb_samples;
    const int dst_samples = swr_get_out_samples(swr_ctx_, src_samples);
    if (dst_samples <= 0) {
        return Result<DecodeResult>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                          "swr_get_out_samples returned invalid");
    }

    const int ch = (dst_channels_ > 0) ? dst_channels_ : av_frame->ch_layout.nb_channels;
    const size_t buf_size = static_cast<size_t>(dst_samples * ch * 2);
    auto buffer = std::make_shared<CpuFrameBuffer>(buf_size);

    uint8_t* out_data = buffer->data();
    const uint8_t* in_planes[8] = {};
    for (int i = 0; i < av_frame->ch_layout.nb_channels && i < 8; ++i) {
        in_planes[i] = av_frame->data[i];
    }

    int converted = swr_convert(swr_ctx_, &out_data, dst_samples, in_planes, src_samples);
    if (converted < 0) {
        return Result<DecodeResult>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                          "swr_convert failed");
    }

    AudioFrame frame;
    frame.format = SampleFormat::S16;
    frame.sample_rate = dst_sample_rate_;
    frame.channels = ch;
    frame.num_samples = converted;
    frame.num_planes = 1;
    frame.planes[0].data = buffer->data();
    frame.planes[0].size = buffer->size();
    frame.planes[0].stride = ch * 2;
    frame.planes[0].offset = 0;
    frame.buffer = std::move(buffer);
    frame.frame_index = frame_index_++;

    if (pts_us >= 0) {
        frame.pts = TimePointUs{pts_us};
    }
    if (av_frame->duration > 0) {
        frame.duration = TimeDeltaUs{
            av_rescale_q(av_frame->duration, codec_tb_, {1, 1'000'000})};
    } else {
        frame.duration = TimeDeltaUs::from_samples(converted, dst_sample_rate_);
    }

    if (frame_index_ % 50 == 0) {
        SB_LOG_I(kTag, "frame#%lld pts_us=%lld q=%zu",
                 static_cast<long long>(frame.frame_index - 1),
                 static_cast<long long>(pts_us), pts_fifo_.size());
    }

    result.has_frame = true;
    result.frame = std::move(frame);
    return Result<DecodeResult>::ok(std::move(result));
}

Result<IAudioDecoder::DecodeResult> FFmpegAudioDecoder::receive_frame() {
    return decode_one_frame();
}

// ============================================================
// IAudioDecoder: drain / flush
// ============================================================

Result<void> FFmpegAudioDecoder::drain() {
    if (codec_ctx_ == nullptr) return Result<void>::ok();

    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return Result<void>::ok();

    // Drain frames (caller should call receive_frame after)
    while (true) {
        auto result = decode_one_frame();
        if (result.is_err() || !result->has_frame) break;
    }
    return Result<void>::ok();
}

void FFmpegAudioDecoder::flush() {
    if (codec_ctx_ != nullptr) {
        avcodec_flush_buffers(codec_ctx_);
    }
    pts_fifo_.clear();
}

}  // namespace streambridge::android::ffmpeg
