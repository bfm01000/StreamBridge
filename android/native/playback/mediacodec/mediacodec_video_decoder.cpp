#include "mediacodec_video_decoder.h"

#include <cstring>

#include "../ffmpeg/ffmpeg_audio_decoder.h"
#include "../ffmpeg/ffmpeg_video_decoder.h"
#include "streambridge/logging.h"

namespace streambridge::android::mediacodec {

static constexpr char kTag[] = "StreamBridgeMC";

MediaCodecVideoDecoder::MediaCodecVideoDecoder() = default;

MediaCodecVideoDecoder::~MediaCodecVideoDecoder() {
    close();
}

// ============================================================
// IVideoDecoder: open / close
// ============================================================

Result<void> MediaCodecVideoDecoder::open(const StreamInfo& info) {
    close();

    width_ = info.width;
    height_ = info.height;

    const char* mime = "video/avc";  // H.264
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (codec_ == nullptr) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                  "MediaCodec: no H.264 decoder");
    }

    // Build format
    AMediaFormatPtr format(AMediaFormat_new());
    AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, width_);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, height_);

    // Set extradata (SPS/PPS) as csd-0
    if (!info.codec_extradata.empty()) {
        AMediaFormat_setBuffer(format.get(), "csd-0",
                               info.codec_extradata.data(),
                               info.codec_extradata.size());
    }

    // Configure with Surface for zero-copy output
    media_status_t status = AMediaCodec_configure(
        codec_, format.get(), surface_, nullptr /* crypto */, 0 /* flags */);
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
    saw_eos_ = false;
    frame_index_ = 0;
    last_queued_pts_us_ = -1;

    SB_LOG_I(kTag, "MediaCodec decoder opened: %dx%d surface=%p",
             width_, height_, static_cast<void*>(surface_));
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
    frame_index_ = 0;
}

Result<void> MediaCodecVideoDecoder::set_surface(ANativeWindow* window) {
    surface_ = window;
    // If already running, recreate with new Surface
    if (codec_ != nullptr && started_) {
        return recreate(window);
    }
    return Result<void>::ok();
}

Result<void> MediaCodecVideoDecoder::recreate(ANativeWindow* new_surface) {
    SB_LOG_I(kTag, "recreating MediaCodec with new surface %p",
             static_cast<void*>(new_surface));

    // Save current format params
    int saved_w = width_;
    int saved_h = height_;

    close();

    StreamInfo info;
    info.codec = CodecId::H264;
    info.width = saved_w;
    info.height = saved_h;

    surface_ = new_surface;
    return open(info);
}

DecodeCapability MediaCodecVideoDecoder::capability() const {
    DecodeCapability cap;
    cap.codec = CodecId::H264;
    cap.is_hardware = true;
    cap.hardware_name = "MediaCodec";
    return cap;
}

// ============================================================
// IVideoDecoder: send_packet
// ============================================================

Result<void> MediaCodecVideoDecoder::send_packet(const MediaPacket& packet) {
    if (codec_ == nullptr || !started_) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                  "decoder not opened");
    }

    auto idx = AMediaCodec_dequeueInputBuffer(codec_, 5000);  // 5ms timeout
    if (idx < 0) {
        // No input buffer available right now — not an error, caller retries
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
// IVideoDecoder: dequeue_output / release_output
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
        // Format changed — get new format, but Surface mode handles this automatically
        auto* fmt = AMediaCodec_getOutputFormat(codec_);
        if (fmt != nullptr) {
            int32_t w = 0, h = 0;
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
            if (w > 0) width_ = w;
            if (h > 0) height_ = h;
            SB_LOG_I(kTag, "output format changed: %dx%d", width_, height_);
        }
        return Result<DecodeOutputInfo>::ok(info);  // no frame, try again
    }

    if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        // Deprecated but handled
        return Result<DecodeOutputInfo>::ok(info);
    }

    if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        return Result<DecodeOutputInfo>::ok(info);
    }

    if (idx < 0) {
        // Unexpected error
        return Result<DecodeOutputInfo>::ok(info);
    }

    // Check for EOS
    if (buf_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        saw_eos_ = true;
        SB_LOG_I(kTag, "EOS received");
        // Release EOS buffer without rendering
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
        return Result<DecodeOutputInfo>::ok(info);
    }

    // Got a valid output buffer — return info without pixel data (zero-copy!)
    info.has_output = true;
    info.pts_us = static_cast<int64_t>(buf_info.presentationTimeUs);
    // durationUs not available in AMediaCodecBufferInfo in all NDK versions
    info.output_index = idx;

    return Result<DecodeOutputInfo>::ok(info);
}

Result<IVideoDecoder::CpuFrameResult> MediaCodecVideoDecoder::receive_frame(
        int /*output_index*/) {
    // Surface mode: no CPU frame available
    CpuFrameResult result;
    result.has_frame = false;
    return Result<CpuFrameResult>::ok(std::move(result));
}

void MediaCodecVideoDecoder::release_output(int output_index, bool render) {
    if (codec_ == nullptr || output_index < 0) return;

    media_status_t status = AMediaCodec_releaseOutputBuffer(
        codec_, static_cast<size_t>(output_index), render);
    if (status == AMEDIA_OK && render) {
        frame_index_++;
        if (frame_index_ % 100 == 0) {
            SB_LOG_I(kTag, "rendered frame#%d pts=%lld",
                     frame_index_, static_cast<long long>(last_queued_pts_us_));
        }
    }
}

// ============================================================
// IVideoDecoder: drain / flush
// ============================================================

Result<void> MediaCodecVideoDecoder::drain() {
    if (codec_ == nullptr || !started_ || saw_eos_) {
        return Result<void>::ok();
    }

    // Send EOS to flush remaining frames
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
    // Try MediaCodec first (hardware path, zero-copy)
    const char* mime = "video/avc";
    AMediaCodec* test_codec = AMediaCodec_createDecoderByType(mime);
    if (test_codec != nullptr) {
        AMediaCodec_delete(test_codec);
        SB_LOG_I(kTag, "using MediaCodec hardware decoder (zero-copy)");
        auto mc = std::make_unique<MediaCodecVideoDecoder>();
        if (surface != nullptr) {
            mc->set_surface(surface);
        }
        return mc;
    }

    // Fallback to FFmpeg software decoder
    SB_LOG_I(kTag, "MediaCodec unavailable, using FFmpeg software decoder");
    return std::make_unique<android::ffmpeg::FFmpegVideoDecoder>();
}

std::unique_ptr<IAudioDecoder> create_audio_decoder() {
    return std::make_unique<android::ffmpeg::FFmpegAudioDecoder>();
}

}  // namespace streambridge::android::mediacodec
