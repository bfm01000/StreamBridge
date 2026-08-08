#include "ffmpeg_rtmp_publisher.h"

extern "C" {
#include <libavutil/opt.h>
}

namespace streambridge {

// FFmpeg interrupt callback
static int interrupt_cb(void* opaque) {
    if (!opaque) return 0;
    auto* token = static_cast<StopToken*>(opaque);
    return token->stop_requested() ? 1 : 0;
}

FFmpegRTMPPublisher::FFmpegRTMPPublisher() = default;

FFmpegRTMPPublisher::~FFmpegRTMPPublisher() {
    close();
}

Result<void> FFmpegRTMPPublisher::open(const PublishConfig& config) {
    config_ = config;

    // 创建 FLV 格式输出 context
    const AVOutputFormat* fmt = av_guess_format("flv", nullptr, nullptr);
    if (!fmt) {
        return Result<void>::err(ErrorDomain::Config, ErrorCode::InvalidConfig,
                                 "FLV muxer not found");
    }

    AVFormatContext* ctx = nullptr;
    int ret = avformat_alloc_output_context2(&ctx, fmt, nullptr, config.url.c_str());
    if (ret < 0 || !ctx) {
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed,
                                 "avformat_alloc_output_context2 failed for: " + config.url);
    }
    fmt_ctx_.reset(ctx);

    // 配置 interrupt callback — 必须先将 token 存为成员，避免临时对象悬垂
    stop_token_ = stop_source_.token();
    AVIOInterruptCB int_cb = {interrupt_cb, stop_token_.as_opaque()};
    ctx->interrupt_callback = int_cb;

    // 打开 IO（avio_open 在 write_header 之前调用）
    ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    // rtmp_live 作为 metadata 在 write_header 时生效
    av_dict_set(&ctx->metadata, "rtmp_live", "live", 0);

    // 最小化 avio_open2: 只用 interrupt callback，不传协议级 option
    // 某些 FFmpeg 版本中 timeout option 会被 TCP 层误解析为 listen_timeout
    ret = avio_open2(&ctx->pb, config.url.c_str(), AVIO_FLAG_WRITE,
                     &ctx->interrupt_callback, nullptr);

    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed,
                                 std::string("Cannot connect: ") + err);
    }

    interrupted_ = false;
    is_open_ = true;
    return Result<void>::ok();
}

void FFmpegRTMPPublisher::setup_stream(const StreamInfo& info, AVStream* stream) {
    AVCodecParameters* par = stream->codecpar;

    if (info.is_video()) {
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = AV_CODEC_ID_H264;
        par->width = info.width;
        par->height = info.height;
        stream->time_base = {1, 1'000'000};  // 微秒
        stream->avg_frame_rate = {static_cast<int>(info.frame_rate), 1};
    } else {
        par->codec_type = AVMEDIA_TYPE_AUDIO;
        par->codec_id = AV_CODEC_ID_AAC;
        par->sample_rate = info.sample_rate;
        par->ch_layout.nb_channels = info.channels;
        stream->time_base = {1, info.sample_rate};  // 采样率 time base
    }

    // Extradata（SPS/PPS 或 AudioSpecificConfig）
    if (!info.codec_extradata.empty()) {
        par->extradata = static_cast<uint8_t*>(av_mallocz(
            info.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(par->extradata, info.codec_extradata.data(),
               info.codec_extradata.size());
        par->extradata_size = static_cast<int>(info.codec_extradata.size());
    }
}

Result<void> FFmpegRTMPPublisher::write_header(const StreamInfo& video_stream,
                                                const StreamInfo& audio_stream) {
    if (!is_open_) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                 "Publisher not open");
    }

    AVFormatContext* ctx = fmt_ctx_.get();

    // 创建视频流
    if (video_stream.is_video()) {
        video_stream_ = avformat_new_stream(ctx, nullptr);
        setup_stream(video_stream, video_stream_);
    }

    // 创建音频流
    if (audio_stream.is_audio()) {
        audio_stream_ = avformat_new_stream(ctx, nullptr);
        setup_stream(audio_stream, audio_stream_);
    }

    // 写头部（FLV header + sequence headers）
    int ret = avformat_write_header(ctx, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkWriteFailed,
                                 std::string("avformat_write_header: ") + err);
    }

    header_written_ = true;
    return Result<void>::ok();
}

Result<void> FFmpegRTMPPublisher::write_packet(const MediaPacket& packet) {
    if (!is_open_ || !header_written_) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                 "Publisher not ready");
    }

    AVFormatContext* ctx = fmt_ctx_.get();
    AVStream* stream = packet.is_video() ? video_stream_ : audio_stream_;

    AVPacket* avpkt = av_packet_alloc();
    if (!avpkt) {
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                 "av_packet_alloc failed");
    }
    avpkt->data = const_cast<uint8_t*>(packet.data.data());
    avpkt->size = static_cast<int>(packet.data.size());
    avpkt->stream_index = stream->index;
    avpkt->flags = packet.is_key_frame ? AV_PKT_FLAG_KEY : 0;

    // 转换时间戳到流 time base
    avpkt->pts = us_to_pts(packet.pts.us, stream->time_base);
    avpkt->dts = us_to_pts(packet.dts.us, stream->time_base);

    if (packet.has_codec_config) {
        avpkt->flags |= AV_PKT_FLAG_KEY;
    }

    int ret = av_interleaved_write_frame(ctx, avpkt);
    av_packet_free(&avpkt);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkWriteFailed,
                                 std::string("av_interleaved_write_frame: ") + err);
    }

    bytes_written_ += packet.data.size();
    packets_written_++;
    return Result<void>::ok();
}

void FFmpegRTMPPublisher::close() {
    if (fmt_ctx_ && fmt_ctx_->pb) {
        if (header_written_) {
            av_write_trailer(fmt_ctx_.get());
        }
        avio_closep(&fmt_ctx_->pb);
    }
    fmt_ctx_.reset();
    video_stream_ = nullptr;
    audio_stream_ = nullptr;
    header_written_ = false;
    is_open_ = false;
}

void FFmpegRTMPPublisher::interrupt() {
    interrupted_ = true;
    stop_source_.request_stop();
}

}  // namespace streambridge
