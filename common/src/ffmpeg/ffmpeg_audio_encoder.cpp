#include "ffmpeg_audio_encoder.h"
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace streambridge {

FFmpegAudioEncoder::FFmpegAudioEncoder() = default;

FFmpegAudioEncoder::~FFmpegAudioEncoder() {
    close();
}

Result<void> FFmpegAudioEncoder::open(const AudioEncodeConfig& config) {
    config_ = config;

    const AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                 "AAC encoder not found");
    }

    codec_ctx_ = alloc_codec_context(codec);
    if (!codec_ctx_) {
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                 "avcodec_alloc_context3 failed");
    }

    AVCodecContext* ctx = codec_ctx_.get();
    ctx->sample_rate = config.sample_rate;
    ctx->sample_fmt = to_av_sample_format(config.input_format);
    ctx->bit_rate = config.bitrate_bps;
    ctx->time_base = {1, config.sample_rate};
    ctx->frame_size = config.frame_size;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 0)
    av_channel_layout_default(&ctx->ch_layout, config.channels);
#else
    ctx->channels = config.channels;
    ctx->channel_layout = config.channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif

    // GLOBAL_HEADER: 把 AudioSpecificConfig 写入 extradata 而非内嵌
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 低延迟
    av_opt_set(ctx->priv_data, "aac_coder", "fast", 0);

    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        codec_ctx_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                 std::string("avcodec_open2 (aac) failed: ") + err);
    }

    packet_ = alloc_packet();
    is_open_ = true;
    frame_count_ = 0;
    return Result<void>::ok();
}

Result<std::vector<MediaPacket>> FFmpegAudioEncoder::encode(AudioFrame frame) {
    if (!is_open_) {
        return Result<std::vector<MediaPacket>>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Encoder not open");
    }

    AVSampleFormat src_fmt = to_av_sample_format(frame.format);

    // 初始化/更新重采样器
    if (!swr_ || swr_in_sample_rate_ != frame.sample_rate || swr_in_fmt_ != src_fmt ||
        acc_channels_ != frame.channels) {
        AVSampleFormat dst_fmt = to_av_sample_format(config_.input_format);
        SwrContext* raw = nullptr;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 0)
        AVChannelLayout in_ch_layout, out_ch_layout;
        av_channel_layout_default(&in_ch_layout, frame.channels);
        av_channel_layout_default(&out_ch_layout, config_.channels);
        swr_alloc_set_opts2(&raw,
            &out_ch_layout, dst_fmt, config_.sample_rate,
            &in_ch_layout, src_fmt, frame.sample_rate,
            0, nullptr);
#else
        int64_t in_layout = frame.channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
        int64_t out_layout = config_.channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
        raw = swr_alloc_set_opts(nullptr,
            out_layout, dst_fmt, config_.sample_rate,
            in_layout, src_fmt, frame.sample_rate,
            0, nullptr);
#endif
        if (!raw || swr_init(raw) < 0) {
            swr_free(&raw);
            return Result<std::vector<MediaPacket>>::err(
                ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                "swr_init failed");
        }
        swr_.reset(raw);
        swr_in_sample_rate_ = frame.sample_rate;
        swr_in_fmt_ = src_fmt;
        acc_format_ = frame.format;
        acc_channels_ = frame.channels;
        acc_samples_ = 0;
        acc_buffer_.clear();
    }

    // 将输入帧追加到累积缓冲
    int bytes_per_sample = av_get_bytes_per_sample(src_fmt);
    int incoming_bytes;
    if (frame.num_planes == 1) {
        // Interleaved: 1 plane
        incoming_bytes = frame.num_samples * frame.channels * bytes_per_sample;
        size_t old_size = acc_buffer_.size();
        acc_buffer_.resize(old_size + incoming_bytes);
        memcpy(acc_buffer_.data() + old_size, frame.planes[0].data, incoming_bytes);
    } else {
        // Planar: 需要交错存放
        for (int s = 0; s < frame.num_samples; s++) {
            for (int ch = 0; ch < frame.channels && ch < frame.num_planes; ch++) {
                const uint8_t* src = frame.planes[ch].data + s * bytes_per_sample;
                acc_buffer_.insert(acc_buffer_.end(), src, src + bytes_per_sample);
            }
        }
        incoming_bytes = frame.num_samples * frame.channels * bytes_per_sample;
    }
    acc_samples_ += frame.num_samples;

    // 尝试消费完整的 1024-sample 帧
    int frame_size = config_.frame_size;  // 1024 for AAC-LC
    std::vector<MediaPacket> all_packets;
    int64_t base_pts = frame_count_;

    while (acc_samples_ >= frame_size) {
        int chunk_bytes = frame_size * acc_channels_ * bytes_per_sample;

        // 构建源 AVFrame（交错格式）
        AVFramePtr src_avf = alloc_frame();
        src_avf->format = src_fmt;
        src_avf->sample_rate = swr_in_sample_rate_;
        src_avf->nb_samples = frame_size;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 0)
        av_channel_layout_default(&src_avf->ch_layout, acc_channels_);
#else
        src_avf->channels = acc_channels_;
        src_avf->channel_layout = acc_channels_ == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif
        src_avf->data[0] = acc_buffer_.data();
        src_avf->linesize[0] = chunk_bytes;

        AVSampleFormat dst_fmt = to_av_sample_format(config_.input_format);
        int dst_nb_samples = av_rescale_rnd(frame_size,
            config_.sample_rate, swr_in_sample_rate_, AV_ROUND_UP);

        AVFramePtr avf = alloc_frame();
        avf->format = dst_fmt;
        avf->sample_rate = config_.sample_rate;
        avf->nb_samples = dst_nb_samples;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 0)
        av_channel_layout_default(&avf->ch_layout, config_.channels);
#else
        avf->channels = config_.channels;
        avf->channel_layout = config_.channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif
        int ret = av_frame_get_buffer(avf.get(), 0);
        if (ret < 0) break;

        int converted = swr_convert(swr_.get(), avf->data, dst_nb_samples,
                                    const_cast<const uint8_t**>(src_avf->data),
                                    frame_size);
        if (converted <= 0) break;
        avf->nb_samples = converted;
        // PTS in sample clock units
        avf->pts = base_pts * frame_size;

        ret = avcodec_send_frame(codec_ctx_.get(), avf.get());
        if (ret >= 0 || ret == AVERROR(EAGAIN)) {
            // 收取编码后的 packets
            AVCodecContext* ctx = codec_ctx_.get();
            while (true) {
                av_packet_unref(packet_.get());
                ret = avcodec_receive_packet(ctx, packet_.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                MediaPacket mp;
                mp.type = MediaType::Audio;
                mp.codec = CodecId::AAC;
                mp.pts.us = av_rescale_q(packet_->pts, {1, config_.sample_rate}, {1, 1'000'000});
                mp.dts = mp.pts;
                mp.data.assign(packet_->data, packet_->data + packet_->size);
                mp.sequence_number = base_pts;
                all_packets.push_back(std::move(mp));
            }
        }

        // 从累积缓冲中移除已消费的采样
        size_t remaining = acc_buffer_.size() - chunk_bytes;
        if (remaining > 0) {
            memmove(acc_buffer_.data(), acc_buffer_.data() + chunk_bytes, remaining);
        }
        acc_buffer_.resize(remaining);
        acc_samples_ -= frame_size;
        base_pts++;
        frame_count_++;
    }

    return Result<std::vector<MediaPacket>>::ok(std::move(all_packets));
}

Result<std::vector<MediaPacket>> FFmpegAudioEncoder::drain() {
    if (!is_open_) {
        return Result<std::vector<MediaPacket>>::ok({});
    }

    int ret = avcodec_send_frame(codec_ctx_.get(), nullptr);
    (void)ret;

    std::vector<MediaPacket> packets;
    AVCodecContext* ctx = codec_ctx_.get();
    while (true) {
        av_packet_unref(packet_.get());
        ret = avcodec_receive_packet(ctx, packet_.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        MediaPacket mp;
        mp.type = MediaType::Audio;
        mp.codec = CodecId::AAC;
        mp.pts.us = av_rescale_q(packet_->pts, {1, config_.sample_rate}, {1, 1'000'000});
        mp.dts = mp.pts;
        mp.data.assign(packet_->data, packet_->data + packet_->size);
        mp.sequence_number = frame_count_;
        packets.push_back(std::move(mp));
    }
    return Result<std::vector<MediaPacket>>::ok(std::move(packets));
}

void FFmpegAudioEncoder::close() {
    swr_.reset();
    converted_frame_.reset();
    packet_.reset();
    codec_ctx_.reset();
    is_open_ = false;
}

CodecCapability FFmpegAudioEncoder::capability() const {
    CodecCapability cap;
    cap.codec = CodecId::AAC;
    cap.is_hardware = false;
    cap.input_sample_formats = {SampleFormat::FLTPlanar, SampleFormat::S16};
    cap.min_bitrate = 32'000;
    cap.max_bitrate = 512'000;
    return cap;
}

std::vector<uint8_t> FFmpegAudioEncoder::extradata() const {
    if (!codec_ctx_ || !codec_ctx_->extradata) return {};
    auto* data = codec_ctx_->extradata;
    return std::vector<uint8_t>(data, data + codec_ctx_->extradata_size);
}

}  // namespace streambridge
