#include "playback_queue_config.h"

#include "playback_constants.h"

namespace streambridge::android {

streambridge::MediaQueue<streambridge::MediaPacket>::Config
compressed_packet_queue_config() {
    streambridge::MediaQueue<streambridge::MediaPacket>::Config config;
    config.max_elements = kMaxPacketQueueSize;
    config.drop_oldest_on_full = false;
    config.push_timeout = streambridge::TimeDeltaUs::from_ms(kDemuxReadTimeoutMs);
    return config;
}

}  // namespace streambridge::android
