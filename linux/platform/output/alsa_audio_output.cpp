#include "alsa_audio_output.h"

#include <cstring>

#include "streambridge/logging.h"

namespace streambridge {

ALSAAudioOutput::~ALSAAudioOutput() {
    close();
}

Result<void> ALSAAudioOutput::open(const std::string& device, int sample_rate, int channels) {
    int ret = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        pcm_ = nullptr;
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string("snd_pcm_open(") + device + ") failed: " + snd_strerror(ret));
    }

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_, params);
    snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, params, static_cast<unsigned>(channels));
    unsigned rate = static_cast<unsigned>(sample_rate);
    snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, nullptr);
    snd_pcm_hw_params_set_period_size(pcm_, params, 960, 0);
    snd_pcm_hw_params_set_buffer_size(pcm_, params, 4800);
    ret = snd_pcm_hw_params(pcm_, params);
    if (ret < 0) {
        SB_LOG_W("output", "hw_params failed (%s), falling back to sw params",
              snd_strerror(ret));
        // 参数放宽：仅要求格式与通道
        snd_pcm_hw_params_any(pcm_, params);
        snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm_, params, static_cast<unsigned>(channels));
        ret = snd_pcm_hw_params(pcm_, params);
        if (ret < 0) {
            SB_LOG_E("output", "hw_params failed: %s", snd_strerror(ret));
            close();
            return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                     std::string("snd_pcm_hw_params: ") + snd_strerror(ret));
        }
    }
    snd_pcm_prepare(pcm_);

    sample_rate_ = static_cast<int>(rate);
    channels_ = channels;
    total_written_ = 0;
    SB_LOG_I("output", "ALSA playback opened: %s %d Hz %d ch", device.c_str(),
          sample_rate_, channels_);
    return Result<void>::ok();
}

void ALSAAudioOutput::close() {
    if (pcm_) {
        snd_pcm_drain(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

Result<int> ALSAAudioOutput::write(const AudioFrame& frame) {
    if (!pcm_) {
        return Result<int>::err(ErrorDomain::Config, ErrorCode::InvalidConfig,
                                "audio output not open");
    }
    if (frame.format != SampleFormat::S16 || frame.num_planes < 1) {
        return Result<int>::err(ErrorDomain::Codec, ErrorCode::CodecFormatUnsupported,
                                "audio output expects S16 interleaved");
    }

    const int16_t* data = reinterpret_cast<const int16_t*>(frame.planes[0].data);
    int remaining = frame.num_samples;  // 单声道采样数（S16 interleaved 单 plane）
    int written_total = 0;

    while (remaining > 0) {
        int ret = snd_pcm_writei(pcm_, data + written_total * channels_, remaining);
        if (ret == -EPIPE) {
            // XRUN（设备缓冲欠载）：恢复并重试
            SB_LOG_W("output", "XRUN (underrun), recovering...");
            snd_pcm_recover(pcm_, ret, 1);
            continue;
        }
        if (ret < 0) {
            return Result<int>::err(ErrorDomain::Device, ErrorCode::NetworkWriteFailed,
                                    std::string("snd_pcm_writei: ") + snd_strerror(ret));
        }
        written_total += ret;
        remaining -= ret;
    }
    total_written_ += written_total;
    return Result<int>::ok(written_total);
}

int64_t ALSAAudioOutput::played_frames() const {
    if (!pcm_) return total_written_;
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm_, &delay) < 0) {
        return total_written_;  // 查询失败时保守回退
    }
    int64_t played = total_written_ - delay;
    return played > 0 ? played : 0;
}

}  // namespace streambridge
