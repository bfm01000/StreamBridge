#include "mediacodec_video_decoder.h"

#include <cstring>

#include "ffmpeg/ffmpeg_audio_decoder.h"
#include "ffmpeg/ffmpeg_video_decoder.h"
#include "mediacodec_csd.h"
#include "streambridge/logging.h"

namespace streambridge::android::mediacodec {

static constexpr char kTag[] = "StreamBridgeMC";

MediaCodecVideoDecoder::MediaCodecVideoDecoder() = default;
MediaCodecVideoDecoder::~MediaCodecVideoDecoder() { close(); }

// ============================================================
// open / close / capability
// ============================================================

Result<void> MediaCodecVideoDecoder::open(const StreamInfo& info) {
    ANativeWindow* saved_surface = surface_;
    close();
    surface_ = saved_surface;

    width_ = info.width;
    height_ = info.height;

    if (info.codec == CodecId::H264) {
        codec_id_ = AV_CODEC_ID_H264;
    } else if (info.codec == CodecId::H265) {
        codec_id_ = AV_CODEC_ID_H265;
    } else {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                  "unsupported codec");
    }

    auto config = streambridge::ffmpeg::parse_codec_config(
        codec_id_, info.codec_extradata.data(), info.codec_extradata.size());
    if (config.is_err())
        return Result<void>::err(config.error_domain(), config.error_code(),
                                  "MediaCodec: " + config.error_message());

    pending_config_ = std::move(*config);
    SB_LOG_I(kTag, "codec=%s extradata=%zu format=%s sps=%zu pps=%zu",
             (codec_id_ == AV_CODEC_ID_H264 ? "H264" : "H265"),
             info.codec_extradata.size(),
             bitstream_format_name(pending_config_.format),
             pending_config_.sps_list.size(), pending_config_.pps_list.size());

    if (pending_config_.is_complete()) return try_finish_configuration();

    config_complete_ = false;
    pending_packets_.clear();
    return Result<void>::ok();
}

Result<void> MediaCodecVideoDecoder::try_finish_configuration() {
    if (!pending_config_.is_complete())
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::InvalidCodecConfig, "not complete");

    auto csd = build_mediacodec_csd(pending_config_);
    const char* mime = (codec_id_ == AV_CODEC_ID_H264) ? "video/avc" : "video/hevc";
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound, "no decoder");

    AMediaFormatPtr format(AMediaFormat_new());
    AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, width_);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, height_);
    if (!csd.csd_0.empty()) AMediaFormat_setBuffer(format.get(), "csd-0", csd.csd_0.data(), csd.csd_0.size());
    if (!csd.csd_1.empty()) AMediaFormat_setBuffer(format.get(), "csd-1", csd.csd_1.data(), csd.csd_1.size());

    SB_LOG_I(kTag, "configure: surface=%p", static_cast<void*>(surface_));
    if (AMediaCodec_configure(codec_, format.get(), surface_, nullptr, 0) != AMEDIA_OK) {
        close();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed, "configure failed");
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        close();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed, "start failed");
    }

    started_ = true;
    config_complete_ = true;
    next_frame_id_ = 1;
    frame_map_.clear();
    SB_LOG_I(kTag, "MediaCodec configured: %dx%d csd0=%zu csd1=%zu", width_, height_, csd.csd_0.size(), csd.csd_1.size());
    return Result<void>::ok();
}

void MediaCodecVideoDecoder::close() {
    if (codec_) {
        if (started_) { AMediaCodec_stop(codec_); started_ = false; }
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    surface_ = nullptr;
    saw_eos_ = false;
    config_complete_ = false;
    frame_map_.clear();
}

DecoderCapability MediaCodecVideoDecoder::capability() const {
    DecoderCapability c;
    c.hardware = true;
    c.supports_surface_output = true;
    c.supports_cpu_output = false;
    return c;
}

Result<void> MediaCodecVideoDecoder::set_surface(ANativeWindow* window) {
    SB_LOG_I(kTag, "set_surface: %p", static_cast<void*>(window));
    surface_ = window;
    if (codec_ && started_) return recreate(window);
    return Result<void>::ok();
}

Result<void> MediaCodecVideoDecoder::recreate(ANativeWindow* new_surface) {
    int sw = width_, sh = height_;
    auto sci = codec_id_;
    close();
    surface_ = new_surface;
    StreamInfo info;
    info.codec = (sci == AV_CODEC_ID_H264) ? CodecId::H264 : CodecId::H265;
    info.width = sw; info.height = sh;
    return open(info);
}

// ============================================================
// send_packet
// ============================================================

Result<DecodeStatus> MediaCodecVideoDecoder::send_packet(const MediaPacket& packet) {
    // Delayed config: accumulate SPS/PPS from packets
    if (!config_complete_) {
        auto r = streambridge::ffmpeg::parse_codec_config_from_packet(
            codec_id_, packet.data.data(), packet.data.size());
        if (r.is_ok()) {
            streambridge::ffmpeg::merge_codec_config(pending_config_, *r);
            if (pending_config_.is_complete()) {
                auto ret = try_finish_configuration();
                if (ret.is_err()) return Result<DecodeStatus>::err(ret.error_domain(), ret.error_code(), ret.error_message());
            } else {
                pending_packets_.insert(pending_packets_.end(), packet.data.begin(), packet.data.end());
                return Result<DecodeStatus>::ok(DecodeStatus::TryAgain);
            }
        }
    }

    if (!codec_ || !started_)
        return Result<DecodeStatus>::err(ErrorDomain::Internal, ErrorCode::InvalidState, "not configured");

    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 50000);
    if (idx < 0) return Result<DecodeStatus>::ok(DecodeStatus::TryAgain);

    size_t buf_size = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, idx, &buf_size);
    if (!buf) return Result<DecodeStatus>::err(ErrorDomain::Internal, ErrorCode::OutOfMemory, "null input buf");

    size_t n = std::min(packet.data.size(), buf_size);
    std::memcpy(buf, packet.data.data(), n);
    int64_t pts = packet.has_valid_pts() ? packet.pts.us : 0;

    if (AMediaCodec_queueInputBuffer(codec_, idx, 0, n, pts, 0) != AMEDIA_OK)
        return Result<DecodeStatus>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed, "queueInputBuffer");

    return Result<DecodeStatus>::ok(DecodeStatus::FrameReady);
}

// ============================================================
// receive_frame
// ============================================================

Result<DecodeOutput> MediaCodecVideoDecoder::receive_frame(int timeout_ms) {
    DecodeOutput out;

    if (!codec_ || !started_ || saw_eos_)
        return Result<DecodeOutput>::err(ErrorDomain::Internal, ErrorCode::QueueTimeout, "not ready");

    AMediaCodecBufferInfo bi;
    auto idx = AMediaCodec_dequeueOutputBuffer(codec_, &bi, timeout_ms * 1000);

    if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        return Result<DecodeOutput>::err(ErrorDomain::Internal, ErrorCode::QueueTimeout, "format changed");
    }
    if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER || idx < 0) {
        return Result<DecodeOutput>::err(ErrorDomain::Internal, ErrorCode::QueueTimeout, "try again");
    }

    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        saw_eos_ = true;
        AMediaCodec_releaseOutputBuffer(codec_, idx, false);
        return Result<DecodeOutput>::err(ErrorDomain::Codec, ErrorCode::PrematureEOF, "EOS");
    }

    out.frame_id = next_frame_id_++;
    out.pts_us = bi.presentationTimeUs;
    out.payload = DecoderSurfaceHandle{};

    frame_map_[out.frame_id] = idx;

    if (out.frame_id <= 5 || out.frame_id % 100 == 0)
        SB_LOG_I(kTag, "frame#%llu pts=%lld idx=%ld",
                 (unsigned long long)out.frame_id, (long long)out.pts_us, idx);

    return Result<DecodeOutput>::ok(std::move(out));
}

// ============================================================
// present / discard
// ============================================================

Result<void> MediaCodecVideoDecoder::present_frame(uint64_t frame_id, int64_t /*target_time_ns*/) {
    auto it = frame_map_.find(frame_id);
    if (it == frame_map_.end())
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidArgument, "unknown frame_id");
    AMediaCodec_releaseOutputBuffer(codec_, it->second, true);
    frame_map_.erase(it);
    return Result<void>::ok();
}

Result<void> MediaCodecVideoDecoder::discard_frame(uint64_t frame_id) {
    auto it = frame_map_.find(frame_id);
    if (it == frame_map_.end())
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidArgument, "unknown frame_id");
    AMediaCodec_releaseOutputBuffer(codec_, it->second, false);
    frame_map_.erase(it);
    return Result<void>::ok();
}

// ============================================================
// drain / flush
// ============================================================

Result<void> MediaCodecVideoDecoder::drain() {
    if (!codec_ || !started_ || saw_eos_) return Result<void>::ok();
    auto idx = AMediaCodec_dequeueInputBuffer(codec_, 50000);
    if (idx < 0) return Result<void>::ok();
    AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    saw_eos_ = true;
    return Result<void>::ok();
}

void MediaCodecVideoDecoder::flush() {
    if (codec_ && started_) { AMediaCodec_flush(codec_); saw_eos_ = false; }
    frame_map_.clear();
}

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IVideoDecoder> create_video_decoder(ANativeWindow* surface) {
    AMediaCodec* tc = AMediaCodec_createDecoderByType("video/avc");
    if (tc) {
        AMediaCodec_delete(tc);
        SB_LOG_I(kTag, "using MediaCodec hardware decoder");
        auto mc = std::make_unique<MediaCodecVideoDecoder>();
        if (surface) mc->set_surface(surface);
        return mc;
    }
    SB_LOG_I(kTag, "MediaCodec unavailable, fallback to FFmpeg");
    return std::make_unique<streambridge::ffmpeg::FFmpegVideoDecoder>();
}

std::unique_ptr<IAudioDecoder> create_audio_decoder() {
    return std::make_unique<streambridge::ffmpeg::FFmpegAudioDecoder>();
}

}  // namespace streambridge::android::mediacodec
