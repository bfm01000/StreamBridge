#include "ffmpeg_subscriber.h"

#include <android/log.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <cstring>

namespace streambridge::android::ffmpeg {
namespace {

constexpr char kLogTag[] = "StreamBridgeSub";

void log_info(const char* msg) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", msg);
}

void log_error(const char* msg) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", msg);
}

// FFmpeg time_base → 微秒
int64_t ts_to_us(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE) return -1;
    return static_cast<int64_t>(ts * av_q2d(tb) * 1'000'000.0);
}

}  // namespace

FFmpegSubscriber::FFmpegSubscriber() = default;

FFmpegSubscriber::~FFmpegSubscriber() {
    close();
}

streambridge::Result<void> FFmpegSubscriber::open(const std::string& url) {
    close();

    if (url.empty()) {
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Config,
            streambridge::ErrorCode::InvalidUrl,
            "empty url");
    }

    // 打开输入（RTMP URL）
    AVFormatContext* ctx = nullptr;
    AVDictionary* opts = nullptr;
    // 设置超时和缓冲区选项，防止阻塞过久
    av_dict_set(&opts, "rtmp_live", "live", 0);
    av_dict_set(&opts, "timeout", "10000000", 0);  // 10s 超时（微秒）

    int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0 || ctx == nullptr) {
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::string msg = "avformat_open_input failed: ";
        msg += errbuf;
        log_error(msg.c_str());
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Network,
            streambridge::ErrorCode::NetworkConnectFailed,
            msg);
    }
    fmt_ctx_ = ctx;

    // 查找流信息
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        close();
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Network,
            streambridge::ErrorCode::NetworkReadFailed,
            std::string("avformat_find_stream_info failed: ") + errbuf);
    }

    // 查找音视频流
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVCodecParameters* par = fmt_ctx_->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ < 0) {
            video_stream_index_ = static_cast<int>(i);
            video_time_base_ = fmt_ctx_->streams[i]->time_base;
            has_video_ = true;

            video_info_.type = streambridge::MediaType::Video;
            video_info_.codec = map_codec_id(par->codec_id);
            video_info_.width = par->width;
            video_info_.height = par->height;
            video_info_.frame_rate = av_q2d(fmt_ctx_->streams[i]->avg_frame_rate);
            if (video_info_.frame_rate <= 0) {
                video_info_.frame_rate = av_q2d(fmt_ctx_->streams[i]->r_frame_rate);
            }
            video_info_.pixel_format = streambridge::PixelFormat::YUV420P;
            video_info_.time_base = {video_time_base_.num, video_time_base_.den};
            video_info_.bitrate_bps = par->bit_rate;
            fill_extradata(par, video_info_);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index_ < 0) {
            audio_stream_index_ = static_cast<int>(i);
            audio_time_base_ = fmt_ctx_->streams[i]->time_base;
            has_audio_ = true;

            audio_info_.type = streambridge::MediaType::Audio;
            audio_info_.codec = map_codec_id(par->codec_id);
            audio_info_.sample_rate = par->sample_rate;
            audio_info_.channels = par->ch_layout.nb_channels;
            audio_info_.sample_format = map_sample_format(static_cast<AVSampleFormat>(par->format));
            audio_info_.time_base = {audio_time_base_.num, audio_time_base_.den};
            audio_info_.bitrate_bps = par->bit_rate;
            fill_extradata(par, audio_info_);
        }
    }

    if (!has_video_ && !has_audio_) {
        close();
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Network,
            streambridge::ErrorCode::NetworkReadFailed,
            "no audio or video stream found in RTMP source");
    }

    log_info("subscriber opened successfully");
    return streambridge::Result<void>::ok();
}

streambridge::Result<streambridge::MediaPacket> FFmpegSubscriber::read_packet() {
    if (fmt_ctx_ == nullptr) {
        return streambridge::Result<streambridge::MediaPacket>::err(
            streambridge::ErrorDomain::Internal,
            streambridge::ErrorCode::InvalidState,
            "subscriber not opened");
    }

    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        return streambridge::Result<streambridge::MediaPacket>::err(
            streambridge::ErrorDomain::Resource,
            streambridge::ErrorCode::OutOfMemory,
            "av_packet_alloc failed");
    }

    int ret = av_read_frame(fmt_ctx_, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        if (ret == AVERROR_EOF) {
            // EOF — 返回空的 MediaPacket 作为信号
            streambridge::MediaPacket empty;
            empty.type = streambridge::MediaType::Unknown;
            return streambridge::Result<streambridge::MediaPacket>::ok(std::move(empty));
        }
        char errbuf[256] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        return streambridge::Result<streambridge::MediaPacket>::err(
            streambridge::ErrorDomain::Network,
            streambridge::ErrorCode::NetworkReadFailed,
            std::string("av_read_frame failed: ") + errbuf);
    }

    streambridge::MediaPacket packet;
    packet.sequence_number = packet_seq_++;

    if (pkt->stream_index == video_stream_index_) {
        packet.type = streambridge::MediaType::Video;
        packet.codec = video_info_.codec;
        packet.pts = streambridge::TimePointUs{ts_to_us(pkt->pts, video_time_base_)};
        packet.dts = streambridge::TimePointUs{ts_to_us(pkt->dts, video_time_base_)};
        packet.duration = streambridge::TimeDeltaUs{ts_to_us(pkt->duration, video_time_base_)};
        packet.is_key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        packet.stream_index = video_stream_index_;
    } else if (pkt->stream_index == audio_stream_index_) {
        packet.type = streambridge::MediaType::Audio;
        packet.codec = audio_info_.codec;
        packet.pts = streambridge::TimePointUs{ts_to_us(pkt->pts, audio_time_base_)};
        packet.dts = streambridge::TimePointUs{ts_to_us(pkt->dts, audio_time_base_)};
        packet.duration = streambridge::TimeDeltaUs{ts_to_us(pkt->duration, audio_time_base_)};
        packet.stream_index = audio_stream_index_;
    } else {
        // 未知流，丢弃
        av_packet_free(&pkt);
        return read_packet();  // 递归读取下一个
    }

    // 复制 packet 数据
    if (pkt->size > 0) {
        packet.data.resize(pkt->size);
        std::memcpy(packet.data.data(), pkt->data, pkt->size);
    }

    av_packet_free(&pkt);
    return streambridge::Result<streambridge::MediaPacket>::ok(std::move(packet));
}

void FFmpegSubscriber::close() {
    if (fmt_ctx_ != nullptr) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
    has_video_ = false;
    has_audio_ = false;
    packet_seq_ = 0;
}

streambridge::CodecId FFmpegSubscriber::map_codec_id(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return streambridge::CodecId::H264;
        case AV_CODEC_ID_AAC:  return streambridge::CodecId::AAC;
        case AV_CODEC_ID_H265: return streambridge::CodecId::H265;
        case AV_CODEC_ID_OPUS: return streambridge::CodecId::Opus;
        default: return streambridge::CodecId::Unknown;
    }
}

streambridge::SampleFormat FFmpegSubscriber::map_sample_format(AVSampleFormat fmt) {
    switch (fmt) {
        case AV_SAMPLE_FMT_S16:  return streambridge::SampleFormat::S16;
        case AV_SAMPLE_FMT_S16P: return streambridge::SampleFormat::S16Planar;
        case AV_SAMPLE_FMT_FLT:  return streambridge::SampleFormat::FLT;
        case AV_SAMPLE_FMT_FLTP: return streambridge::SampleFormat::FLTPlanar;
        default: return streambridge::SampleFormat::Unknown;
    }
}

void FFmpegSubscriber::fill_extradata(AVCodecParameters* par, streambridge::StreamInfo& info) {
    if (par->extradata != nullptr && par->extradata_size > 0) {
        info.codec_extradata.resize(par->extradata_size);
        std::memcpy(info.codec_extradata.data(), par->extradata, par->extradata_size);
    }
}

}  // namespace streambridge::android::ffmpeg
