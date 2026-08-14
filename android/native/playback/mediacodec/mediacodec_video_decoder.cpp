#include "mediacodec_video_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "ffmpeg/ffmpeg_audio_decoder.h"
#include "ffmpeg/ffmpeg_video_decoder.h"
#include "mediacodec_csd.h"
#include "streambridge/logging.h"

namespace streambridge::android::mediacodec {

static constexpr char kTag[] = "StreamBridgeMC";
static constexpr int kImageReaderMaxImages = 4;

const char* media_status_name(media_status_t status);

struct MediaCodecReleaseState {
    std::mutex mutex;
    AMediaCodec* codec = nullptr;
    bool active = false;
};

class MediaCodecOutputBufferLease final : public streambridge::DecodedFrameLease {
public:
    MediaCodecOutputBufferLease(
            std::shared_ptr<MediaCodecReleaseState> state,
            int buffer_index)
        : state_(std::move(state))
        , buffer_index_(buffer_index) {}

    ~MediaCodecOutputBufferLease() override {
        (void)release(false, 0);
    }

    Result<void> present(int64_t target_time_ns) override {
        return release(true, target_time_ns);
    }

    Result<void> discard() override {
        return release(false, 0);
    }

private:
    Result<void> release(bool render, int64_t target_time_ns) {
        if (released_) {
            return Result<void>::ok();
        }
        released_ = true;

        auto state = state_;
        if (!state) {
            return Result<void>::ok();
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->active || state->codec == nullptr || buffer_index_ < 0) {
            return Result<void>::ok();
        }

        media_status_t status = AMEDIA_OK;
        if (render && target_time_ns > 0) {
            status = AMediaCodec_releaseOutputBufferAtTime(
                state->codec, buffer_index_, target_time_ns);
        } else {
            status = AMediaCodec_releaseOutputBuffer(
                state->codec, buffer_index_, render);
        }
        if (status != AMEDIA_OK) {
            return Result<void>::err(
                ErrorDomain::Codec,
                ErrorCode::CodecDecodeFailed,
                std::string("release output buffer failed status=") +
                    media_status_name(status));
        }
        return Result<void>::ok();
    }

    std::shared_ptr<MediaCodecReleaseState> state_;
    int buffer_index_ = -1;
    bool released_ = false;
};

class AImageFrameLease final : public streambridge::DecodedFrameLease {
public:
    explicit AImageFrameLease(AImage* image) : image_(image) {}

    ~AImageFrameLease() override {
        release_image();
    }

    Result<void> present(int64_t) override {
        release_image();
        return Result<void>::ok();
    }

    Result<void> discard() override {
        release_image();
        return Result<void>::ok();
    }

private:
    void release_image() {
        if (image_ != nullptr) {
            AImage_delete(image_);
            image_ = nullptr;
        }
    }

    AImage* image_ = nullptr;
};

bool is_try_again_status(media_status_t status) {
    return status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE ||
           status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED;
}

const char* media_status_name(media_status_t status) {
    switch (status) {
        case AMEDIA_OK:
            return "AMEDIA_OK";
        case AMEDIA_ERROR_UNKNOWN:
            return "AMEDIA_ERROR_UNKNOWN";
        case AMEDIA_ERROR_MALFORMED:
            return "AMEDIA_ERROR_MALFORMED";
        case AMEDIA_ERROR_UNSUPPORTED:
            return "AMEDIA_ERROR_UNSUPPORTED";
        case AMEDIA_ERROR_INVALID_OBJECT:
            return "AMEDIA_ERROR_INVALID_OBJECT";
        case AMEDIA_ERROR_INVALID_PARAMETER:
            return "AMEDIA_ERROR_INVALID_PARAMETER";
        case AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE:
            return "AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE";
        case AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED:
            return "AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED";
        default:
            return "AMEDIA_STATUS_UNKNOWN";
    }
}

MediaCodecVideoDecoder::MediaCodecVideoDecoder() = default;
MediaCodecVideoDecoder::~MediaCodecVideoDecoder() { close(); }

// ============================================================
// open / close / capability
// ============================================================

Result<void> MediaCodecVideoDecoder::open(const StreamInfo& info) {
    ANativeWindow* saved_surface = surface_;
    close();
    surface_ = saved_surface;
    stream_info_ = info;

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

    ANativeWindow* output_surface = surface_;
    output_mode_ = OutputMode::Surface;
    if (preferred_path_ != VideoDecodePath::MediaCodecSurface) {
        auto image_reader_result = create_image_reader();
        if (image_reader_result.is_ok()) {
            output_surface = image_reader_window_;
            output_mode_ = OutputMode::HardwareBuffer;
        } else if (preferred_path_ == VideoDecodePath::MediaCodecAhbGpu) {
            close_image_reader();
            close();
            return Result<void>::err(
                image_reader_result.error_domain(), image_reader_result.error_code(),
                "forced AHardwareBuffer output unavailable: " +
                    image_reader_result.error_message());
        } else {
            SB_LOG_W(kTag, "AHardwareBuffer output unavailable, fallback to Surface: %s",
                     image_reader_result.error_message().c_str());
            close_image_reader();
        }
    }

    if (output_surface == nullptr) {
        close();
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceNotFound,
                                  "MediaCodec requires a valid output surface");
    }

    SB_LOG_I(kTag, "configure output=%s surface=%p",
             output_mode_ == OutputMode::HardwareBuffer ? "AHardwareBuffer" : "Surface",
             static_cast<void*>(output_surface));
    if (AMediaCodec_configure(codec_, format.get(), output_surface, nullptr, 0) != AMEDIA_OK) {
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
    release_state_ = std::make_shared<MediaCodecReleaseState>();
    {
        std::lock_guard<std::mutex> lock(release_state_->mutex);
        release_state_->codec = codec_;
        release_state_->active = true;
    }
    SB_LOG_I(kTag, "MediaCodec configured: %dx%d csd0=%zu csd1=%zu mode=%s",
             width_, height_, csd.csd_0.size(), csd.csd_1.size(),
             output_mode_ == OutputMode::HardwareBuffer ? "AHardwareBuffer" : "Surface");
    return Result<void>::ok();
}

void MediaCodecVideoDecoder::close() {
    if (release_state_) {
        std::lock_guard<std::mutex> lock(release_state_->mutex);
        release_state_->active = false;
        release_state_->codec = nullptr;
    }
    if (codec_) {
        if (started_) { AMediaCodec_stop(codec_); started_ = false; }
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    saw_eos_ = false;
    config_complete_ = false;
    release_state_.reset();
    close_image_reader();
}

DecoderCapability MediaCodecVideoDecoder::capability() const {
    DecoderCapability c;
    c.hardware = true;
    c.supports_surface_output = true;
    c.supports_cpu_output = true;
    return c;
}

Result<void> MediaCodecVideoDecoder::set_surface(ANativeWindow* window) {
    SB_LOG_I(kTag, "set_surface: %p", static_cast<void*>(window));
    surface_ = window;
    if (codec_ && started_) return recreate(window);
    return Result<void>::ok();
}

void MediaCodecVideoDecoder::set_preferred_output_path(VideoDecodePath path) {
    preferred_path_ = path;
}

Result<void> MediaCodecVideoDecoder::recreate(ANativeWindow* new_surface) {
    streambridge::StreamInfo saved_info = stream_info_;
    close();
    surface_ = new_surface;
    return open(saved_info);
}

Result<void> MediaCodecVideoDecoder::create_image_reader() {
    if (width_ <= 0 || height_ <= 0) {
        return Result<void>::err(ErrorDomain::Config, ErrorCode::InvalidArgument,
                                 "invalid image reader geometry");
    }

    close_image_reader();

    const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    image_reader_cpu_readable_ =
        (usage & (AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN)) != 0;
    media_status_t status = AImageReader_newWithUsage(
        width_,
        height_,
        AIMAGE_FORMAT_YUV_420_888,
        usage,
        kImageReaderMaxImages,
        &image_reader_);
    if (status != AMEDIA_OK || image_reader_ == nullptr) {
        SB_LOG_W(kTag,
                 "AImageReader_newWithUsage failed status=%s(%d), retry AImageReader_new",
                 media_status_name(status), static_cast<int>(status));
        status = AImageReader_new(
            width_,
            height_,
            AIMAGE_FORMAT_YUV_420_888,
            kImageReaderMaxImages,
            &image_reader_);
        image_reader_cpu_readable_ = true;
        if (status != AMEDIA_OK || image_reader_ == nullptr) {
            return Result<void>::err(
                ErrorDomain::Device,
                ErrorCode::DeviceCapUnsupported,
                std::string("AImageReader_new failed status=") +
                    media_status_name(status));
        }
    }

    status = AImageReader_getWindow(image_reader_, &image_reader_window_);
    if (status != AMEDIA_OK || image_reader_window_ == nullptr) {
        close_image_reader();
        return Result<void>::err(
            ErrorDomain::Device,
            ErrorCode::DeviceCapUnsupported,
            std::string("AImageReader_getWindow failed status=") +
                media_status_name(status));
    }

    image_acquire_count_ = 0;
    SB_LOG_I(kTag,
             "AImageReader created %dx%d YUV_420_888 max=%d usage=0x%llx cpu_readable=%d window=%p",
             width_, height_, kImageReaderMaxImages,
             static_cast<unsigned long long>(usage),
             image_reader_cpu_readable_ ? 1 : 0,
             static_cast<void*>(image_reader_window_));
    return Result<void>::ok();
}

void MediaCodecVideoDecoder::close_image_reader() {
    image_reader_window_ = nullptr;
    if (image_reader_ != nullptr) {
        AImageReader_delete(image_reader_);
        image_reader_ = nullptr;
    }
}

Result<AImage*> MediaCodecVideoDecoder::acquire_next_image() {
    if (image_reader_ == nullptr) {
        return Result<AImage*>::err(
            ErrorDomain::Device,
            ErrorCode::InvalidState,
            "AImageReader is null");
    }

    AImage* image = nullptr;
    media_status_t status = AImageReader_acquireNextImage(image_reader_, &image);
    if (status == AMEDIA_OK && image != nullptr) {
        ++image_acquire_count_;
        if (image_acquire_count_ <= 5 || image_acquire_count_ % 120 == 0) {
            SB_LOG_I(kTag,
                     "AImageReader acquire next count=%lld",
                     static_cast<long long>(image_acquire_count_));
        }
        return Result<AImage*>::ok(image);
    }
    if (is_try_again_status(status)) {
        return Result<AImage*>::err(
            ErrorDomain::Internal,
            ErrorCode::QueueTimeout,
            "AImageReader output not ready");
    }
    return Result<AImage*>::err(
        ErrorDomain::Device,
        ErrorCode::CodecDecodeFailed,
        std::string("AImageReader_acquireNextImage failed status=") +
            media_status_name(status));
}

Result<streambridge::VideoFrame> MediaCodecVideoDecoder::image_to_video_frame(
        AImage* image,
        AHardwareBuffer* hardware_buffer,
        int64_t pts_us) {
    if (image == nullptr || hardware_buffer == nullptr) {
        return Result<streambridge::VideoFrame>::err(
            ErrorDomain::Device, ErrorCode::InvalidArgument,
            "AImage/AHardwareBuffer is null");
    }

    auto wrapped = HardwareBufferFrameBuffer::wrap(
        hardware_buffer, streambridge::PixelFormat::YUV420P);
    if (wrapped.is_err()) {
        return Result<streambridge::VideoFrame>::err(
            wrapped.error_domain(), wrapped.error_code(), wrapped.error_message());
    }

    streambridge::VideoFrame frame;
    frame.width = width_;
    frame.height = height_;
    frame.pts = streambridge::TimePointUs{pts_us};
    frame.buffer = *wrapped;
    frame.format = streambridge::PixelFormat::Unknown;
    frame.num_planes = 0;

    if (!image_reader_cpu_readable_) {
        return Result<streambridge::VideoFrame>::ok(frame);
    }

    int plane_count = 0;
    media_status_t status = AImage_getNumberOfPlanes(image, &plane_count);
    if (status != AMEDIA_OK || plane_count < 2) {
        SB_LOG_W(kTag, "AImage has no CPU-readable planes; use AHardwareBuffer GPU path");
        return Result<streambridge::VideoFrame>::ok(frame);
    }

    const int usable_planes = std::min(plane_count, streambridge::VideoFrame::kMaxPlanes);

    for (int i = 0; i < usable_planes; ++i) {
        uint8_t* data = nullptr;
        int data_length = 0;
        int row_stride = 0;
        status = AImage_getPlaneData(image, i, &data, &data_length);
        if (status != AMEDIA_OK || data == nullptr || data_length <= 0) {
            SB_LOG_W(kTag,
                     "AImage plane %d is not CPU-readable; use AHardwareBuffer GPU path",
                     i);
            frame.num_planes = 0;
            return Result<streambridge::VideoFrame>::ok(frame);
        }
        status = AImage_getPlaneRowStride(image, i, &row_stride);
        if (status != AMEDIA_OK || row_stride <= 0) {
            SB_LOG_W(kTag,
                     "AImage plane %d row stride unavailable; use AHardwareBuffer GPU path",
                     i);
            frame.num_planes = 0;
            return Result<streambridge::VideoFrame>::ok(frame);
        }
        frame.planes[i].data = data;
        frame.planes[i].stride = row_stride;
        frame.planes[i].size = static_cast<size_t>(data_length);
        frame.planes[i].offset = 0;
    }
    frame.num_planes = usable_planes;

    int pixel_stride_u = 0;
    int pixel_stride_v = 0;
    if (plane_count >= 3 &&
            AImage_getPlanePixelStride(image, 1, &pixel_stride_u) == AMEDIA_OK &&
            AImage_getPlanePixelStride(image, 2, &pixel_stride_v) == AMEDIA_OK &&
            pixel_stride_u == 1 && pixel_stride_v == 1) {
        frame.format = streambridge::PixelFormat::YUV420P;
        frame.num_planes = 3;
    } else if (plane_count >= 3 &&
            AImage_getPlanePixelStride(image, 1, &pixel_stride_u) == AMEDIA_OK &&
            pixel_stride_u == 2) {
        frame.format = streambridge::PixelFormat::NV12;
        frame.num_planes = 2;
    } else if (plane_count == 2) {
        frame.format = streambridge::PixelFormat::NV12;
        frame.num_planes = 2;
    } else {
        return Result<streambridge::VideoFrame>::err(
            ErrorDomain::Device, ErrorCode::CodecFormatUnsupported,
            "unsupported YUV_420_888 plane layout");
    }

    return Result<streambridge::VideoFrame>::ok(frame);
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

    if (packet.data.size() > buf_size) {
        return Result<DecodeStatus>::err(ErrorDomain::Codec, ErrorCode::CodecDecodeFailed,
                                         "MediaCodec input buffer too small");
    }

    size_t n = packet.data.size();
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

    if (output_mode_ == OutputMode::HardwareBuffer && image_reader_ != nullptr) {
        AMediaCodec_releaseOutputBuffer(codec_, idx, true);

        Result<AImage*> acquired = Result<AImage*>::err(
            ErrorDomain::Internal, ErrorCode::QueueTimeout, "not ready");
        for (int attempt = 0; attempt < 3; ++attempt) {
            acquired = acquire_next_image();
            if (acquired.is_ok()) {
                break;
            }
            if (acquired.error_code() != ErrorCode::QueueTimeout) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (acquired.is_err()) {
            return Result<DecodeOutput>::err(
                acquired.error_domain(), acquired.error_code(),
                acquired.error_message());
        }
        AImage* image = *acquired;

        AHardwareBuffer* hardware_buffer = nullptr;
        media_status_t status = AImage_getHardwareBuffer(image, &hardware_buffer);
        if (status != AMEDIA_OK || hardware_buffer == nullptr) {
            AImage_delete(image);
            return Result<DecodeOutput>::err(
                ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                std::string("AImage_getHardwareBuffer failed status=") +
                    media_status_name(status));
        }

        auto frame = image_to_video_frame(image, hardware_buffer, out.pts_us);
        if (frame.is_err()) {
            AImage_delete(image);
            return Result<DecodeOutput>::err(
                frame.error_domain(), frame.error_code(), frame.error_message());
        }

        out.payload = HardwareBufferFrameHandle{std::move(*frame)};
        out.lease = std::make_shared<AImageFrameLease>(image);
    } else {
        out.payload = DecoderSurfaceHandle{};
        out.lease = std::make_shared<MediaCodecOutputBufferLease>(
            release_state_, static_cast<int>(idx));
    }

    if (out.frame_id <= 5 || out.frame_id % 100 == 0)
        SB_LOG_I(kTag, "frame#%llu pts=%lld idx=%ld",
                 (unsigned long long)out.frame_id, (long long)out.pts_us, idx);

    return Result<DecodeOutput>::ok(std::move(out));
}

// ============================================================
// present / discard
// ============================================================

Result<void> MediaCodecVideoDecoder::present_frame(uint64_t frame_id, int64_t target_time_ns) {
    (void)frame_id;
    (void)target_time_ns;
    return Result<void>::err(
        ErrorDomain::Internal,
        ErrorCode::InvalidState,
        "MediaCodec frame lifecycle is owned by DecodeOutput::lease");
}

Result<void> MediaCodecVideoDecoder::discard_frame(uint64_t frame_id) {
    (void)frame_id;
    return Result<void>::err(
        ErrorDomain::Internal,
        ErrorCode::InvalidState,
        "MediaCodec frame lifecycle is owned by DecodeOutput::lease");
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
}

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IVideoDecoder> create_video_decoder(
        ANativeWindow* surface,
        VideoDecodePath path) {
    if (path == VideoDecodePath::FFmpegSoftware) {
        SB_LOG_I(kTag, "using FFmpeg software decoder (forced)");
        return std::make_unique<streambridge::ffmpeg::FFmpegVideoDecoder>();
    }

    AMediaCodec* probe = AMediaCodec_createDecoderByType("video/avc");
    if (probe != nullptr && surface != nullptr) {
        AMediaCodec_delete(probe);
        auto decoder = std::make_unique<MediaCodecVideoDecoder>();
        decoder->set_preferred_output_path(path);
        decoder->set_surface(surface);
        SB_LOG_I(kTag, "using MediaCodec hardware decoder path=%s",
                 video_decode_path_name(path));
        return decoder;
    }
    if (probe != nullptr) {
        AMediaCodec_delete(probe);
    }
    SB_LOG_I(kTag, "using FFmpeg software decoder fallback");
    return std::make_unique<streambridge::ffmpeg::FFmpegVideoDecoder>();
}

Result<void> set_decoder_surface(streambridge::IVideoDecoder* decoder, ANativeWindow* surface) {
    if (decoder == nullptr || !decoder->capability().supports_surface_output) {
        return Result<void>::ok();
    }
    return static_cast<MediaCodecVideoDecoder*>(decoder)->set_surface(surface);
}

std::unique_ptr<IAudioDecoder> create_audio_decoder() {
    return std::make_unique<streambridge::ffmpeg::FFmpegAudioDecoder>();
}

}  // namespace streambridge::android::mediacodec
