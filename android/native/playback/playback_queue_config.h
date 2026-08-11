#pragma once

#include "streambridge/media_queue.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

streambridge::MediaQueue<streambridge::MediaPacket>::Config
compressed_packet_queue_config();

}  // namespace streambridge::android
