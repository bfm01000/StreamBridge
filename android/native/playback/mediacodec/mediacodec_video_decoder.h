#pragma once
// MediaCodec H.264 video decoder — implements common IVideoDecoder (Surface mode)
// Zero-copy: decoded frames go directly to ANativeWindow Surface via GPU

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstdint>

#include "mediacodec_raii.h"
#include "streambridge/codec.h"
#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android::mediacodec {

class MediaCodecVideoDecoder : public streambridge::IVideoDecoder {
public:
    MediaCodecVideoDecoder();
    ~MediaCodecVideoDecoder() override;

    MediaCodecVideoDecoder(const MediaCodecVideoDecoder&) = delete;
    MediaCodecVideoDecoder& operator=(const MediaCodecVideoDecoder&) = delete;

    // --- IVideoDecoder interface ---
    Result<void> open(const StreamInfo& info) override;
    void close() override;
    bool is_open() const override { return codec_ != nullptr; }

    Result<void> send_packet(const MediaPacket& packet) override;
    Result<DecodeOutputInfo> dequeue_output(int64_t timeout_us) override;
    void release_output(int output_index, bool render) override;
    Result<void> drain() override;
    void flush() override;

    OutputMode output_mode() const override { return OutputMode::Surface; }
    DecodeCapability capability() const override;

    // CPU mode: not supported for Surface output
    Result<CpuFrameResult> receive_frame(int output_index) override;

    // Surface mode: update the output Surface (e.g., after activity recreate)
    Result<void> set_surface(ANativeWindow* window);

private:
    // Configure the codec with current format and surface
    Result<void> configure_with_surface(ANativeWindow* surface);
    // Rebuild the codec (e.g., after surface change)
    Result<void> recreate(ANativeWindow* new_surface);

    AMediaCodec* codec_ = nullptr;
    ANativeWindow* surface_ = nullptr;  // borrowed, not owned
    int width_ = 0;
    int height_ = 0;
    int frame_index_ = 0;
    bool started_ = false;
    bool saw_eos_ = false;

    // Input tracking: queue of pending PTS values (MediaCodec preserves PTS ordering)
    // We track locally for diagnostic logging
    int64_t last_queued_pts_us_ = -1;
};

// Factory: prefer MediaCodec hardware, fallback to FFmpeg software
// surface can be nullptr (pass it later via set_surface for MediaCodec)
std::unique_ptr<streambridge::IVideoDecoder> create_video_decoder(
    ANativeWindow* surface);

// Also: factory for audio (always FFmpeg for now)
std::unique_ptr<streambridge::IAudioDecoder> create_audio_decoder();

}  // namespace streambridge::android::mediacodec
