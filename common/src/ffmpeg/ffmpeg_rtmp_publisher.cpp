#include "ffmpeg_rtmp_publisher.h"
#include "streambridge/logging.h"

#include <vector>

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

namespace {

void write_be24(AVIOContext* pb, uint32_t value) {
    avio_w8(pb, static_cast<int>((value >> 16) & 0xFF));
    avio_w8(pb, static_cast<int>((value >> 8) & 0xFF));
    avio_w8(pb, static_cast<int>(value & 0xFF));
}

void write_be32(AVIOContext* pb, uint32_t value) {
    avio_w8(pb, static_cast<int>((value >> 24) & 0xFF));
    avio_w8(pb, static_cast<int>((value >> 16) & 0xFF));
    avio_w8(pb, static_cast<int>((value >> 8) & 0xFF));
    avio_w8(pb, static_cast<int>(value & 0xFF));
}

int find_start_code(const uint8_t* data, size_t size, size_t offset, int& start_len) {
    for (size_t i = offset; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start_len = 3;
            return static_cast<int>(i);
        }
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            start_len = 4;
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::vector<uint8_t>> split_annexb_nals(const uint8_t* data, size_t size) {
    std::vector<std::vector<uint8_t>> nals;
    size_t offset = 0;
    while (offset < size) {
        int sc_len = 0;
        int sc_pos = find_start_code(data, size, offset, sc_len);
        if (sc_pos < 0) {
            break;
        }
        size_t nal_start = static_cast<size_t>(sc_pos + sc_len);
        int next_len = 0;
        int next_pos = find_start_code(data, size, nal_start, next_len);
        size_t nal_end = next_pos >= 0 ? static_cast<size_t>(next_pos) : size;
        if (nal_end > nal_start) {
            nals.emplace_back(data + nal_start, data + nal_end);
        }
        offset = nal_end;
    }
    return nals;
}

int h264_nal_type(const std::vector<uint8_t>& nal) {
    return nal.empty() ? 0 : (nal[0] & 0x1F);
}

std::vector<uint8_t> make_avcc_extradata(const std::vector<uint8_t>& annexb_config) {
    auto nals = split_annexb_nals(annexb_config.data(), annexb_config.size());
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    for (const auto& nal : nals) {
        int type = h264_nal_type(nal);
        if (type == 7 && sps.empty()) {
            sps = nal;
        } else if (type == 8 && pps.empty()) {
            pps = nal;
        }
    }
    if (sps.empty() || pps.empty()) {
        return {};
    }

    std::vector<uint8_t> avcc;
    avcc.reserve(11 + sps.size() + pps.size());
    avcc.push_back(1);
    avcc.push_back(sps.size() > 1 ? sps[1] : 0x42);
    avcc.push_back(sps.size() > 2 ? sps[2] : 0x00);
    avcc.push_back(sps.size() > 3 ? sps[3] : 0x1F);
    avcc.push_back(0xFF);
    avcc.push_back(0xE1);
    avcc.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);
    avcc.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    return avcc;
}

std::vector<uint8_t> annexb_packet_to_avcc(const uint8_t* data, size_t size) {
    auto nals = split_annexb_nals(data, size);
    if (nals.empty()) {
        return std::vector<uint8_t>(data, data + size);
    }

    std::vector<uint8_t> out;
    for (const auto& nal : nals) {
        int type = h264_nal_type(nal);
        if (type == 7 || type == 8 || type == 9) {
            continue;
        }
        uint32_t len = static_cast<uint32_t>(nal.size());
        out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

void write_flv_tag(AVIOContext* pb,
                   uint8_t tag_type,
                   uint32_t timestamp_ms,
                   const std::vector<uint8_t>& payload) {
    avio_w8(pb, tag_type);
    write_be24(pb, static_cast<uint32_t>(payload.size()));
    write_be24(pb, timestamp_ms & 0xFFFFFF);
    avio_w8(pb, static_cast<int>((timestamp_ms >> 24) & 0xFF));
    write_be24(pb, 0);
    if (!payload.empty()) {
        avio_write(pb, payload.data(), static_cast<int>(payload.size()));
    }
    write_be32(pb, static_cast<uint32_t>(11 + payload.size()));
}

}  // namespace

FFmpegRTMPPublisher::FFmpegRTMPPublisher() = default;

FFmpegRTMPPublisher::~FFmpegRTMPPublisher() {
    close();
}

Result<void> FFmpegRTMPPublisher::open(const PublishConfig& config) {
    config_ = config;
    stop_source_.reset();
    stop_token_ = stop_source_.token();
    AVIOInterruptCB int_cb = {interrupt_cb, stop_token_.as_opaque()};

    // 创建 FLV 格式输出 context
    const AVOutputFormat* fmt = av_guess_format("flv", nullptr, nullptr);
    if (!fmt) {
        int ret = avio_open2(&raw_pb_, config.url.c_str(), AVIO_FLAG_WRITE, &int_cb, nullptr);
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, err, sizeof(err));
            return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed,
                                     std::string("Cannot connect raw FLV RTMP: ") + err);
        }
        raw_flv_mode_ = true;
        interrupted_ = false;
        is_open_ = true;
        return Result<void>::ok();
    }

    AVFormatContext* ctx = nullptr;
    int ret = avformat_alloc_output_context2(&ctx, fmt, nullptr, config.url.c_str());
    if (ret < 0 || !ctx) {
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed,
                                 "avformat_alloc_output_context2 failed for: " + config.url);
    }
    fmt_ctx_.reset(ctx);

    // 配置 interrupt callback — 必须先将 token 存为成员，避免临时对象悬垂
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

    if (raw_flv_mode_) {
        if (!video_stream.is_video()) {
            return Result<void>::err(ErrorDomain::Config, ErrorCode::InvalidConfig,
                                     "raw FLV fallback requires video stream");
        }
        std::vector<uint8_t> avcc = make_avcc_extradata(video_stream.codec_extradata);
        if (avcc.empty()) {
            return Result<void>::err(ErrorDomain::Codec, ErrorCode::InvalidCodecConfig,
                                     "raw FLV fallback missing H.264 SPS/PPS");
        }

        const uint8_t flags = audio_stream.is_audio() ? 5 : 1;
        const uint8_t header[] = {'F', 'L', 'V', 1, flags, 0, 0, 0, 9, 0, 0, 0, 0};
        avio_write(raw_pb_, header, static_cast<int>(sizeof(header)));

        std::vector<uint8_t> payload;
        payload.reserve(5 + avcc.size());
        payload.push_back(0x17);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);
        payload.insert(payload.end(), avcc.begin(), avcc.end());
        write_flv_tag(raw_pb_, 9, 0, payload);

        if (audio_stream.is_audio()) {
            if (audio_stream.codec_extradata.empty()) {
                return Result<void>::err(ErrorDomain::Codec, ErrorCode::InvalidCodecConfig,
                                         "raw FLV fallback missing AAC AudioSpecificConfig");
            }
            std::vector<uint8_t> audio_payload;
            audio_payload.reserve(2 + audio_stream.codec_extradata.size());
            audio_payload.push_back(0xAF);  // AAC, 44kHz tag hint, 16-bit, stereo-compatible
            audio_payload.push_back(0);     // AAC sequence header
            audio_payload.insert(audio_payload.end(),
                                 audio_stream.codec_extradata.begin(),
                                 audio_stream.codec_extradata.end());
            write_flv_tag(raw_pb_, 8, 0, audio_payload);
        }
        avio_flush(raw_pb_);

        raw_video_stream_ = video_stream;
        raw_audio_stream_ = audio_stream;
        header_written_ = true;
        return Result<void>::ok();
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

    if (raw_flv_mode_) {
        if (packet.is_audio()) {
            if (packet.data.empty()) {
                return Result<void>::ok();
            }
            const int64_t pts_us = packet.pts.us >= 0 ? packet.pts.us : 0;
            const uint32_t timestamp_ms =
                pts_us > 0 ? static_cast<uint32_t>(pts_us / 1000) : 0;
            std::vector<uint8_t> payload;
            payload.reserve(2 + packet.data.size());
            payload.push_back(0xAF);
            payload.push_back(1);  // AAC raw frame
            payload.insert(payload.end(), packet.data.begin(), packet.data.end());
            write_flv_tag(raw_pb_, 8, timestamp_ms, payload);
            avio_flush(raw_pb_);

            if (raw_pb_->error < 0) {
                char err[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(raw_pb_->error, err, sizeof(err));
                return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkWriteFailed,
                                         std::string("raw FLV audio write: ") + err);
            }
            bytes_written_ += static_cast<int64_t>(payload.size() + 15);
            packets_written_++;
            return Result<void>::ok();
        }
        if (!packet.is_video()) {
            return Result<void>::ok();
        }
        std::vector<uint8_t> avcc_payload =
            annexb_packet_to_avcc(packet.data.data(), packet.data.size());
        if (avcc_payload.empty()) {
            return Result<void>::ok();
        }

        int64_t dts_us = packet.dts.us >= 0 ? packet.dts.us : packet.pts.us;
        int64_t cts_us = packet.pts.us - dts_us;
        uint32_t timestamp_ms = dts_us > 0 ? static_cast<uint32_t>(dts_us / 1000) : 0;
        int32_t cts_ms = static_cast<int32_t>(cts_us / 1000);

        std::vector<uint8_t> payload;
        payload.reserve(5 + avcc_payload.size());
        payload.push_back(packet.is_key_frame ? 0x17 : 0x27);
        payload.push_back(1);
        payload.push_back(static_cast<uint8_t>((cts_ms >> 16) & 0xFF));
        payload.push_back(static_cast<uint8_t>((cts_ms >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(cts_ms & 0xFF));
        payload.insert(payload.end(), avcc_payload.begin(), avcc_payload.end());
        write_flv_tag(raw_pb_, 9, timestamp_ms, payload);
        avio_flush(raw_pb_);

        if (raw_pb_->error < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(raw_pb_->error, err, sizeof(err));
            return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkWriteFailed,
                                     std::string("raw FLV write: ") + err);
        }
        bytes_written_ += static_cast<int64_t>(payload.size() + 15);
        packets_written_++;
        return Result<void>::ok();
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
    if (raw_pb_) {
        avio_flush(raw_pb_);
        avio_closep(&raw_pb_);
    }
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
    raw_flv_mode_ = false;
    is_open_ = false;
}

void FFmpegRTMPPublisher::interrupt() {
    interrupted_ = true;
    stop_source_.request_stop();
}

}  // namespace streambridge
