#pragma once

#include <cstddef>
#include <cstdint>

namespace streambridge::android {

constexpr std::size_t kMaxPacketQueueSize = 60;
constexpr int64_t kPacketPopTimeoutMs = 200;
constexpr int64_t kDemuxReadTimeoutMs = 5000;
constexpr int kVideoDrainTimeoutMs = 0;

}  // namespace streambridge::android
