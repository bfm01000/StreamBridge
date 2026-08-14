#include "ffmpeg_subscriber.h"

#include "codec_config.h"
#include "streambridge/ffmpeg_utils.h"
#include "streambridge/logging.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <cstring>
#include <limits>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>

namespace {

// 保留为调试工具：测试原生 POSIX socket 连通性
__attribute__((unused))
static bool test_posix_connect(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        SB_LOG_E("StreamBridgeSub",
                "POSIX socket() failed: %s (%d)", strerror(errno), errno);
        return false;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        SB_LOG_E("StreamBridgeSub",
                "inet_pton failed: %s (%d)", strerror(errno), errno);
        close(fd);
        return false;
    }
    int ret = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(fd);
    if (ret < 0) {
        SB_LOG_E("StreamBridgeSub",
                "POSIX connect() failed: %s (%d)", strerror(errno), errno);
        return false;
    }
    SB_LOG_I("StreamBridgeSub",
            "POSIX connect OK: %s:%d fd=%d", host, port, fd);
    return true;
}

}  // namespace

namespace streambridge::ffmpeg {
namespace {

constexpr char kLogTag[] = "StreamBridgeSub";

void log_info(const char* msg) {
    SB_LOG_I(kLogTag, "%s", msg);
}

void log_error(const char* msg) {
    SB_LOG_E(kLogTag, "%s", msg);
}

uint32_t read_be_length(const uint8_t* data, int length_size) {
    uint32_t value = 0;
    for (int i = 0; i < length_size; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

bool convert_length_prefixed_nals_to_annexb(std::vector<uint8_t>& data,
                                            int length_size) {
    if (length_size != 1 && length_size != 2 && length_size != 4) {
        return false;
    }
    if (data.size() < static_cast<size_t>(length_size)) {
        return false;
    }

    std::vector<uint8_t> converted;
    converted.reserve(data.size() + 16);

    size_t offset = 0;
    int nal_count = 0;
    while (offset + static_cast<size_t>(length_size) <= data.size()) {
        const uint32_t nal_size = read_be_length(data.data() + offset, length_size);
        offset += static_cast<size_t>(length_size);
        if (nal_size == 0 ||
                nal_size > data.size() - offset ||
                nal_size > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            return false;
        }

        converted.push_back(0x00);
        converted.push_back(0x00);
        converted.push_back(0x00);
        converted.push_back(0x01);
        converted.insert(converted.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                         data.begin() + static_cast<std::ptrdiff_t>(offset + nal_size));
        offset += nal_size;
        ++nal_count;
    }

    if (offset != data.size() || nal_count == 0) {
        return false;
    }

    data.swap(converted);
    return true;
}

// FFmpeg time_base → 微秒
int64_t ts_to_us(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE) return -1;
    return static_cast<int64_t>(ts * av_q2d(tb) * 1'000'000.0);
}

}  // namespace

// FFmpeg IO 中断回调：StopToken 请求停止时让 av_read_frame 立即返回
static int subscriber_interrupt_cb(void* opaque) {
    if (!opaque) return 0;
    auto* token = static_cast<streambridge::StopToken*>(opaque);
    return token->stop_requested() ? 1 : 0;
}

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
    avformat_network_init();

    AVFormatContext* ctx = nullptr;
    // 不传任何选项：Android FFmpeg 最小化构建对选项敏感，
    // timeout/rtmp_live 等选项可能导致内部 TCP 连接失败 (EADDRNOTAVAIL)
    int ret = avformat_open_input(&ctx, url.c_str(), nullptr, nullptr);
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

    // 挂接 IO 中断回调：stop 或 reconnect 前调用 interrupt() 可唤醒阻塞读
    stop_token_ = stop_source_.token();
    AVIOInterruptCB int_cb = {subscriber_interrupt_cb, stop_token_.as_opaque()};
    fmt_ctx_->interrupt_callback = int_cb;

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

            SB_LOG_I(kLogTag,
                    "video stream: codec=%d %dx%d fps=%.2f tb=%d/%d",
                    static_cast<int>(par->codec_id),
                    par->width, par->height,
                    video_info_.frame_rate,
                    video_time_base_.num, video_time_base_.den);
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

            SB_LOG_I(kLogTag,
                    "audio stream: codec=%d %dHz %dch tb=%d/%d",
                    static_cast<int>(par->codec_id),
                    par->sample_rate, par->ch_layout.nb_channels,
                    audio_time_base_.num, audio_time_base_.den);
        }
    }

    if (!has_video_ && !has_audio_) {
        close();
        return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Network,
            streambridge::ErrorCode::NetworkReadFailed,
            "no audio or video stream found in RTMP source");
    }

    // Detect if container uses length-prefixed NALs → need inline Annex-B conversion
    if (has_video_ && !video_info_.codec_extradata.empty()) {
        auto detect = detect_bitstream_format(
            (video_info_.codec == CodecId::H265) ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264,
            video_info_.codec_extradata.data(),
            video_info_.codec_extradata.size());
        if (detect == BitstreamFormat::Avcc || detect == BitstreamFormat::Hvcc) {
            auto parsed_config = parse_codec_config(
                (video_info_.codec == CodecId::H265) ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264,
                video_info_.codec_extradata.data(),
                video_info_.codec_extradata.size());
            if (parsed_config.is_err()) {
                close();
                return streambridge::Result<void>::err(
                    parsed_config.error_domain(),
                    parsed_config.error_code(),
                    parsed_config.error_message());
            }
            need_annexb_conversion_ = true;
            nal_length_size_ = std::max(1, std::min(4, parsed_config->nal_length_size));
            SB_LOG_I(kLogTag,
                    "subscriber: Annex-B conversion enabled (container=%s nal_len_size=%d)",
                    bitstream_format_name(detect), nal_length_size_);
        }
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
        // PTS 诊断：前 5 个视频包打印原始 PTS
        if (packet_seq_ <= 5) {
            SB_LOG_I(kLogTag,
                    "video raw pkt#%lld pts=%lld dts=%lld tb=%d/%d key=%d",
                    static_cast<long long>(packet_seq_),
                    static_cast<long long>(pkt->pts),
                    static_cast<long long>(pkt->dts),
                    video_time_base_.num, video_time_base_.den,
                    (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
        }
        packet.type = streambridge::MediaType::Video;
        packet.codec = video_info_.codec;
        packet.pts = streambridge::TimePointUs{ts_to_us(pkt->pts, video_time_base_)};
        packet.dts = streambridge::TimePointUs{ts_to_us(pkt->dts, video_time_base_)};
        packet.duration = streambridge::TimeDeltaUs{ts_to_us(pkt->duration, video_time_base_)};
        packet.is_key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        packet.stream_index = video_stream_index_;
    } else if (pkt->stream_index == audio_stream_index_) {
        // PTS 诊断：前 5 个音频包打印原始 PTS
        if (packet_seq_ <= 5) {
            SB_LOG_I(kLogTag,
                    "audio raw pkt#%lld pts=%lld dts=%lld tb=%d/%d",
                    static_cast<long long>(packet_seq_),
                    static_cast<long long>(pkt->pts),
                    static_cast<long long>(pkt->dts),
                    audio_time_base_.num, audio_time_base_.den);
        }
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

    // Convert length-prefixed NALs → Annex-B start-code (avcC/hvcC containers).
    // A single FLV/MP4 packet can contain multiple NAL units:
    // [len][NAL][len][NAL]... → [00 00 00 01][NAL][00 00 00 01][NAL]...
    if (packet.type == streambridge::MediaType::Video && need_annexb_conversion_) {
        const size_t before_size = packet.data.size();
        if (!convert_length_prefixed_nals_to_annexb(packet.data, nal_length_size_)) {
            return streambridge::Result<streambridge::MediaPacket>::err(
                streambridge::ErrorDomain::Codec,
                streambridge::ErrorCode::MalformedAvcc,
                "failed to convert length-prefixed video packet to Annex-B");
        }
        if (packet.sequence_number < 5) {
            SB_LOG_I(kLogTag,
                    "annexb pkt#%lld converted %zu -> %zu bytes",
                    static_cast<long long>(packet.sequence_number),
                    before_size, packet.data.size());
        }
    }

    return streambridge::Result<streambridge::MediaPacket>::ok(std::move(packet));
}

void FFmpegSubscriber::close() {
    need_annexb_conversion_ = false;
    nal_length_size_ = 4;
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

void FFmpegSubscriber::interrupt() {
    stop_source_.request_stop();
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

}  // namespace streambridge::ffmpeg
