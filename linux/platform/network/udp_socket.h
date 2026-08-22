#pragma once
// Linux UDP socket RAII wrapper. Linux-specific fd/socket details stay in platform layer.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "streambridge/media_errors.h"

namespace streambridge::linux_platform {

struct UdpDatagram {
    std::vector<uint8_t> data;
    std::string remote_host;
    uint16_t remote_port = 0;
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    Result<void> open();
    Result<void> bind(uint16_t port, const std::string& bind_host = "0.0.0.0");
    Result<size_t> send_to(const uint8_t* data, size_t size,
                           const std::string& host, uint16_t port);
    Result<UdpDatagram> recv_datagram(size_t max_size);

    void close();
    bool is_open() const { return fd_.load() >= 0; }
    int fd() const { return fd_.load(); }
    uint16_t local_port() const;

private:
    int release() noexcept;
    std::atomic<int> fd_{-1};
};

}  // namespace streambridge::linux_platform

