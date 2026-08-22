#pragma once
// Android UDP receiver RAII wrapper for RTP video playback path.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "streambridge/media_errors.h"

namespace streambridge::android {

struct AndroidUdpDatagram {
    std::vector<uint8_t> data;
};

class AndroidUdpReceiver {
public:
    AndroidUdpReceiver() = default;
    ~AndroidUdpReceiver();

    AndroidUdpReceiver(const AndroidUdpReceiver&) = delete;
    AndroidUdpReceiver& operator=(const AndroidUdpReceiver&) = delete;

    Result<void> bind(uint16_t local_port);
    Result<AndroidUdpDatagram> recv_datagram(size_t max_size);
    void close();
    bool is_open() const { return fd_ >= 0; }
    uint16_t local_port() const { return local_port_; }

private:
    int fd_ = -1;
    uint16_t local_port_ = 0;
};

}  // namespace streambridge::android
