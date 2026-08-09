#include "ffmpeg_audio_decoder.h"

#include <android/log.h>

#include "ffmpeg_raii.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace streambridge::android::ffmpeg {
namespace {
// (internal helpers)
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

    // 查找 AAC 解码器
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

    // 设置 extradata（AudioSpecificConfig）
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

    ctx->sample_rate = info.sample_rate;
    AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&ctx->ch_layout, &stereo_layout);
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
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
    dst_sample_rate_ = info.sample_rate;
    dst_channels_ = info.channels;
    frame_index_ = 0;

    // 初始化重采样器（FLTP → S16 interleaved）
    AVChannelLayout out_ch_layout;
    AVChannelLayout stereo_layout2 = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&out_ch_layout, &stereo_layout2);

    int ret_swr = swr_alloc_set_opts2(&swr_ctx_,
                                       &out_ch_layout, AV_SAMPLE_FMT_S16, dst_sample_rate_,
                                       &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
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

streambridge::Result<FFmpegAudioDecoder::DecodeResult> FFmpegAudioDecoder::decode(
        const streambridge::MediaPacket& packet) {
    if (codec_ctx_ == nullptr || swr_ctx_ == nullptr) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "audio decoder not opened");
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

    return frame_to_audio_frame(av_frame.get());
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

    return frame_to_audio_frame(av_frame.get());
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
}

streambridge::Result<FFmpegAudioDecoder::DecodeResult>
FFmpegAudioDecoder::frame_to_audio_frame(const AVFrame* av_frame) {
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
    // 计算目标样本数（可能有微小差异）
    const int dst_samples = swr_get_out_samples(swr_ctx_, src_samples);
    if (dst_samples <= 0) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            "swr_get_out_samples returned invalid value");
    }

    // 分配输出 buffer（S16 interleaved: 2 bytes × channels × samples）
    const int dst_channels = (dst_channels_ > 0) ? dst_channels_ : av_frame->ch_layout.nb_channels;
    const size_t buffer_size = static_cast<size_t>(dst_samples * dst_channels * 2);
    auto buffer = std::make_shared<streambridge::CpuFrameBuffer>(buffer_size);

    uint8_t* out_data = buffer->data();
    const uint8_t* const* in_data = const_cast<const uint8_t**>(av_frame->data);

    // 执行重采样
    int converted = swr_convert(swr_ctx_, &out_data, dst_samples,
                                in_data, src_samples);
    if (converted < 0) {
        return streambridge::Result<DecodeResult>::err(
            streambridge::ErrorDomain::Codec,
            streambridge::ErrorCode::CodecDecodeFailed,
            "swr_convert failed");
    }

    // 构建 AudioFrame
    streambridge::AudioFrame frame;
    frame.format = streambridge::SampleFormat::S16;
    frame.sample_rate = dst_sample_rate_;
    frame.channels = dst_channels;
    frame.num_samples = converted;
    frame.num_planes = 1;
    frame.planes[0].data = buffer->data();
    frame.planes[0].size = buffer->size();
    frame.planes[0].stride = dst_channels * 2;  // S16 interleaved stride
    frame.planes[0].offset = 0;
    frame.buffer = std::move(buffer);
    frame.frame_index = frame_index_++;

    // PTS
    if (av_frame->pts != AV_NOPTS_VALUE) {
        frame.pts = streambridge::TimePointUs{av_frame->pts};
    }
    if (av_frame->duration > 0) {
        frame.duration = streambridge::TimeDeltaUs{av_frame->duration};
    } else {
        frame.duration = streambridge::TimeDeltaUs::from_samples(converted, dst_sample_rate_);
    }

    DecodeResult result;
    result.has_frame = true;
    result.frame = std::move(frame);
    return streambridge::Result<DecodeResult>::ok(std::move(result));
}

}  // namespace streambridge::android::ffmpeg
