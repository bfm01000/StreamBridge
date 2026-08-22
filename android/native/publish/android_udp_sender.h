#pragma once
// Android UDP sender RAII wrapper for RTP video publish path.

#include <cstddef>
#include <cstdint>
#include <string>

#include "streambridge/media_errors.h"

namespace streambridge::android {

class AndroidUdpSender {
public:
    AndroidUdpSender() = default;
    ~AndroidUdpSender();

    AndroidUdpSender(const AndroidUdpSender&) = delete;
    AndroidUdpSender& operator=(const AndroidUdpSender&) = delete;

    Result<void> open(uint16_t local_port = 0);
    Result<size_t> send_to(const uint8_t* data, size_t size,
                           const std::string& host, uint16_t port);
    void close();
    bool is_open() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace streambridge::android
