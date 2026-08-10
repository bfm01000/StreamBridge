#include "mediacodec_video_decoder.h"

#include <cstring>

#include "ffmpeg/ffmpeg_audio_decoder.h"
#include "ffmpeg/ffmpeg_video_decoder.h"
#include "mediacodec_csd.h"
#include "streambridge/logging.h"

namespace streambridge::android::mediacodec {

static constexpr char kTag[] = "StreamBridgeMC";

MediaCodecVideoDecoder::MediaCodecVideoDecoder() = default;

MediaCodecVideoDecoder::~MediaCodecVideoDecoder() {
    close();
}

// ============================================================
// IVideoDecoder: open
// ============================================================

Result<void> MediaCodecVideoDecoder::open(const StreamInfo& info) {
    close();

    width_ = info.width;
    height_ = info.height;

    // Determine codec type
    if (info.codec == CodecId::H264) {
        codec_id_ = AV_CODEC_ID_H264;
    } else if (info.codec == CodecId::H265) {
        codec_id_ = AV_CODEC_ID_H265;
    } else {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                  "unsupported codec for MediaCodec");
    }

    // Parse extradata
    auto config_result = streambridge::ffmpeg::parse_codec_config(
        codec_id_, info.codec_extradata.data(), info.codec_extradata.size());

    if (config_result.is_err()) {
        return Result<void>::err(config_result.error_domain(),
                                  config_result.error_code(),
                                  "MediaCodec: " + config_result.error_message());
    }

    pending_config_ = std::move(*config_result);
    SB_LOG_I(kTag, "codec=%s extradata=%zu format=%s sps=%zu pps=%zu vps=%zu",
             (codec_id_ == AV_CODEC_ID_H264 ? "H264" : "H265"),
             info.codec_extradata.size(),
             bitstream_format_name(pending_config_.format),
             pending_config_.sps_list.size(),
             pending_config_.pps_list.size(),
             pending_config_.vps_list.size());

    if (pending_config_.is_complete()) {
        return try_finish_configuration();
    }

    // Bare Annex-B: no extradata, wait for SPS/PPS in packets
    config_complete_ = false;
    pending_packets_.clear();
    SB_LOG_I(kTag, "MediaCodec: waiting for parameter sets from bitstream...");
    return Result<void>::ok();
}

Result<void> MediaCodecVideoDecoder::try_finish_configuration() {
    if (!pending_config_.is_complete()) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::InvalidCodecConfig,
                                  "MediaCodec: config not complete");
    }

    // Build CSD
    auto csd = build_mediacodec_csd(pending_config_);

    // Create codec
    const char* mime = (codec_id_ == AV_CODEC_ID_H264) ? "video/avc" : "video/hevc";
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (codec_ == nullptr) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                  "MediaCodec: cannot create decoder");
    }

    AMediaFormatPtr format(AMediaFormat_new());
    AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, width_);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, height_);

    if (!csd.csd_0.empty()) {
        AMediaFormat_setBuffer(format.get(), "csd-0",
                               csd.csd_0.data(), csd.csd_0.size());
    }
    if (!csd.csd_1.empty()) {
        AMediaFormat_setBuffer(format.get(), "csd-1",
                               csd.csd_1.data(), csd.csd_1.size());
    }

    media_status_t status = AMediaCodec_configure(
        codec_, format.get(), surface_, nullptr, 0);
    if (status != AMEDIA_OK) {
        close();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                  "MediaCodec: configure failed");
    }

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        close();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                  "MediaCodec: start failed");
    }

    started_ = true;
    config_complete_ = true;

    // Feed any buffered packets from pre-config phase
    SB_LOG_I(kTag, "MediaCodec configured: %dx%d csd0=%zu csd1=%zu",
             width_, height_, csd.csd_0.size(), csd.csd_1.size());

    if (!pending_packets_.empty()) {
        SB_LOG_I(kTag, "feeding %zu buffered packets", pending_packets_.size());
        // Packets were buffered raw; we'll feed them as MediaPacket in send_packet
        // Simplified: pending_packets_ is concatenated raw Annex-B data, feed as one chunk
        AMediaCodecBufferInfo buf_info{};
        buf_info.size = pending_packets_.size();
        auto idx = AMediaCodec_dequeueInputBuffer(codec_, 5000);
        if (idx >= 0) {
            size_t buf_size = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(codec_,
                static_cast<size_t>(idx), &buf_size);
            if (buf != nullptr && pending_packets_.size() <= buf_size) {
                std::memcpy(buf, pending_packets_.data(), pending_packets_.size());
                AMediaCodec_queueInputBuffer(codec_,
                    static_cast<size_t>(idx), 0,
                    pending_packets_.size(), 0, 0);
            }
        }
        pending_packets_.clear();
    }

    return Result<void>::ok();
}

void MediaCodecVideoDecoder::close() {
    if (codec_ != nullptr) {
        if (started_) {
            AMediaCodec_stop(codec_);
            started_ = false;
        }
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    surface_ = nullptr;
    saw_eos_ = false;
    config_complete_ = false;
    codec_id_ = AV_CODEC_ID_NONE;
    pending_config_ = {};
    pending_packets_.clear();
    frame_index_ = 0;
}

Result<void> MediaCodecVideoDecoder::set_surface(ANativeWindow* window) {
    surface_ = window;
    if (codec_ != nullptr && started_) {
        return recreate(window);
    }
    return Result<void>::ok();
}

Result<void> MediaCodecVideoDecoder::recreate(ANativeWindow* new_surface) {
    SB_LOG_I(kTag, "recreating MediaCodec with new surface %p",
             static_cast<void*>(new_surface));
    int saved_w = width_;
    int saved_h = height_;
    auto saved_codec_id = codec_id_;

    close();

    StreamInfo info;
    info.codec = (saved_codec_id == AV_CODEC_ID_H264) ? CodecId::H264 : CodecId::H265;
    info.width = saved_w;
    info.height = saved_h;

    surface_ = new_surface;
    return open(info);
}

DecodeCapability MediaCodecVideoDecoder::capability() const {
    DecodeCapability cap;
    cap.codec = (codec_id_ == AV_CODEC_ID_H264) ? CodecId::H264 : CodecId::H265;
    cap.is_hardware = true;
    cap.hardware_name = "MediaCodec";
    return cap;
}

// ============================================================
// IVideoDecoder: send_packet
// ============================================================

Result<void> MediaCodecVideoDecoder::send_packet(const MediaPacket& packet) {
    if (!config_complete_) {
        // Try to extract parameter sets from packet
        auto pack_result = streambridge::ffmpeg::parse_codec_config_from_packet(
            codec_id_, packet.data.data(), packet.data.size());

        if (pack_result.is_ok()) {
            auto& pack_cfg = *pack_result;
            if (!pack_cfg.sps_list.empty() || !pack_cfg.pps_list.empty() ||
                    !pack_cfg.vps_list.empty()) {
                streambridge::ffmpeg::merge_codec_config(pending_config_, pack_cfg);

                if (pending_config_.is_complete()) {
                    SB_LOG_I(kTag, "parameter sets extracted from packet, configuring now");
                    auto ret = try_finish_configuration();
                    if (ret.is_err()) return ret;
                    // Fall through to feed this packet normally
                } else {
                    // Still incomplete, buffer packet data
                    pending_packets_.insert(pending_packets_.end(),
                                             packet.data.begin(), packet.data.end());
                    return Result<void>::ok();
                }
            } else {
                // No parameter sets in this packet, buffer it
                pending_packets_.insert(pending_packets_.end(),
                                         packet.data.begin(), packet.data.end());
                return Result<void>::ok();
            }
        }
    }

    if (codec_ == nullptr || !started_) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                  "decoder not configured");
    }

    auto idx = AMediaCodec_dequeueInputBuffer(codec_, 5000);
    if (idx < 0) {
        return Result<void>::ok();
    }

    size_t buf_size = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &buf_size);
    if (buf == nullptr) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::OutOfMemory,
                                  "MediaCodec: null input buffer");
    }

    size_t copy_size = std::min(packet.data.size(), buf_size);
    if (copy_size > 0) {
        std::memcpy(buf, packet.data.data(), copy_size);
    }

    uint32_t flags = 0;
    int64_t pts_us = packet.has_valid_pts() ? packet.pts.us : 0;
    last_queued_pts_us_ = pts_us;

    media_status_t status = AMediaCodec_queueInputBuffer(
        codec_, static_cast<size_t>(idx), 0, copy_size,
        static_cast<uint64_t>(pts_us), flags);
    if (status != AMEDIA_OK) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                  "MediaCodec: queueInputBuffer failed");
    }

    return Result<void>::ok();
}

// ============================================================
// IVideoDecoder: dequeue_output / release_output / receive_frame
// ============================================================

Result<DecodeOutputInfo> MediaCodecVideoDecoder::dequeue_output(int64_t timeout_us) {
    DecodeOutputInfo info;

    if (codec_ == nullptr || !started_) {
        return Result<DecodeOutputInfo>::ok(info);
    }
    if (saw_eos_) {
        return Result<DecodeOutputInfo>::ok(info);
    }

    AMediaCodecBufferInfo buf_info;
    auto idx = AMediaCodec_dequeueOutputBuffer(
        codec_, &buf_info, static_cast<int64_t>(timeout_us));

    if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        auto* fmt = AMediaCodec_getOutputFormat(codec_);
        if (fmt != nullptr) {
            int32_t w = 0, h = 0;
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
            if (w > 0) width_ = w;
            if (h > 0) height_ = h;
        }
        return Result<DecodeOutputInfo>::ok(info);
    }

    if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED ||
            idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER || idx < 0) {
        return Result<DecodeOutputInfo>::ok(info);
    }

    if (buf_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        saw_eos_ = true;
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
        return Result<DecodeOutputInfo>::ok(info);
    }

    info.has_output = true;
    info.pts_us = static_cast<int64_t>(buf_info.presentationTimeUs);
    info.output_index = idx;
    return Result<DecodeOutputInfo>::ok(info);
}

void MediaCodecVideoDecoder::release_output(int output_index, bool render) {
    if (codec_ == nullptr || output_index < 0) return;
    AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(output_index), render);
    if (render) {
        frame_index_++;
        if (frame_index_ % 100 == 0) {
            SB_LOG_I(kTag, "rendered frame#%d pts=%lld",
                     frame_index_, static_cast<long long>(last_queued_pts_us_));
        }
    }
}

Result<IVideoDecoder::CpuFrameResult> MediaCodecVideoDecoder::receive_frame(int) {
    CpuFrameResult result;
    result.has_frame = false;
    return Result<CpuFrameResult>::ok(std::move(result));
}

// ============================================================
// IVideoDecoder: drain / flush
// ============================================================

Result<void> MediaCodecVideoDecoder::drain() {
    if (codec_ == nullptr || !started_ || saw_eos_) {
        return Result<void>::ok();
    }
    auto idx = AMediaCodec_dequeueInputBuffer(codec_, 5000);
    if (idx < 0) return Result<void>::ok();
    size_t buf_size = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &buf_size);
    if (buf == nullptr) return Result<void>::ok();
    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0,
                                  AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    saw_eos_ = true;
    return Result<void>::ok();
}

void MediaCodecVideoDecoder::flush() {
    if (codec_ != nullptr && started_) {
        AMediaCodec_flush(codec_);
        saw_eos_ = false;
    }
}

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IVideoDecoder> create_video_decoder(ANativeWindow* surface) {
    const char* mime = "video/avc";
    AMediaCodec* test_codec = AMediaCodec_createDecoderByType(mime);
    if (test_codec != nullptr) {
        AMediaCodec_delete(test_codec);
        SB_LOG_I(kTag, "using MediaCodec hardware decoder (zero-copy)");
        auto mc = std::make_unique<MediaCodecVideoDecoder>();
        if (surface != nullptr) mc->set_surface(surface);
        return mc;
    }
    SB_LOG_I(kTag, "MediaCodec unavailable, using FFmpeg software decoder");
    return std::make_unique<streambridge::ffmpeg::FFmpegVideoDecoder>();
}

std::unique_ptr<IAudioDecoder> create_audio_decoder() {
    return std::make_unique<streambridge::ffmpeg::FFmpegAudioDecoder>();
}

}  // namespace streambridge::android::mediacodec
