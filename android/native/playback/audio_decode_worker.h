#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "native_audio_output.h"
#include "playback_metrics.h"
#include "streambridge/av_sync.h"
#include "streambridge/codec.h"
#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"
#include "streambridge/session.h"

namespace streambridge::android {

class AudioDecodeWorker {
public:
    struct Callbacks {
        std::function<bool()> is_stopping;
        std::function<streambridge::SessionState()> state;
        std::function<bool(streambridge::StreamInfo&)> copy_audio_info;
        std::function<void(const std::string&)> on_error;
    };

    AudioDecodeWorker(streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue,
                      std::atomic<int64_t>& first_audio_pts_us,
                      streambridge::MediaClock& clock,
                      PlaybackMetrics& metrics,
                      Callbacks callbacks);

    void run();
    void close();

private:
    bool wait_for_audio_info(streambridge::StreamInfo& info);

    streambridge::MediaQueue<streambridge::MediaPacket>& audio_queue_;
    std::atomic<int64_t>& first_audio_pts_us_;
    streambridge::MediaClock& clock_;
    PlaybackMetrics& metrics_;
    Callbacks callbacks_;
    std::unique_ptr<streambridge::IAudioDecoder> audio_decoder_;
    NativeAudioOutput audio_output_;
};

}  // namespace streambridge::android
