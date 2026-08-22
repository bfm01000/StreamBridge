#include "network/udp_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace streambridge::linux_platform {
namespace {

std::string errno_message(const char* prefix) {
    std::string message(prefix);
    message += ": ";
    message += std::strerror(errno);
    return message;
}

Result<sockaddr_in> make_ipv4_addr(const std::string& host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return Result<sockaddr_in>::err(
            ErrorDomain::Config,
            ErrorCode::InvalidConfig,
            "UDP socket currently expects an IPv4 numeric address");
    }
    return Result<sockaddr_in>::ok(addr);
}

}  // namespace

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept {
    fd_.store(other.release());
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_.store(other.release());
    }
    return *this;
}

Result<void> UdpSocket::open() {
    if (is_open()) {
        return Result<void>::ok();
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    fd_.store(fd);
    if (fd < 0) {
        return Result<void>::err(
            ErrorDomain::Network, ErrorCode::NetworkConnectFailed, errno_message("socket"));
    }

    int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        const std::string message = errno_message("setsockopt(SO_REUSEADDR)");
        close();
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed, message);
    }
    return Result<void>::ok();
}

Result<void> UdpSocket::bind(uint16_t port, const std::string& bind_host) {
    auto opened = open();
    if (opened.is_err()) {
        return opened;
    }
    auto addr = make_ipv4_addr(bind_host, port);
    if (addr.is_err()) {
        return Result<void>::err(addr.error_domain(), addr.error_code(), addr.error_message());
    }
    const int fd = fd_.load();
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&(*addr)), sizeof(sockaddr_in)) != 0) {
        return Result<void>::err(
            ErrorDomain::Network, ErrorCode::NetworkConnectFailed, errno_message("bind"));
    }
    return Result<void>::ok();
}

Result<size_t> UdpSocket::send_to(const uint8_t* data, size_t size,
                                  const std::string& host, uint16_t port) {
    if (data == nullptr || size == 0) {
        return Result<size_t>::err(
            ErrorDomain::Internal, ErrorCode::InvalidArgument, "empty UDP payload");
    }
    auto opened = open();
    if (opened.is_err()) {
        return Result<size_t>::err(opened.error_domain(), opened.error_code(), opened.error_message());
    }
    auto addr = make_ipv4_addr(host, port);
    if (addr.is_err()) {
        return Result<size_t>::err(addr.error_domain(), addr.error_code(), addr.error_message());
    }

    const int fd = fd_.load();
    const ssize_t sent = ::sendto(fd, data, size, 0,
                                  reinterpret_cast<const sockaddr*>(&(*addr)),
                                  sizeof(sockaddr_in));
    if (sent < 0) {
        return Result<size_t>::err(
            ErrorDomain::Network, ErrorCode::NetworkWriteFailed, errno_message("sendto"));
    }
    return Result<size_t>::ok(static_cast<size_t>(sent));
}

Result<UdpDatagram> UdpSocket::recv_datagram(size_t max_size) {
    if (!is_open()) {
        return Result<UdpDatagram>::err(
            ErrorDomain::Network, ErrorCode::NetworkReadFailed, "UDP socket is not open");
    }
    if (max_size == 0) {
        return Result<UdpDatagram>::err(
            ErrorDomain::Internal, ErrorCode::InvalidArgument, "max_size must be non-zero");
    }

    int fd = fd_.load();
    while (fd >= 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int poll_result = ::poll(&pfd, 1, 100);
        if (fd_.load() < 0) {
            return Result<UdpDatagram>::err(
                ErrorDomain::Network, ErrorCode::NetworkDisconnected, "UDP socket closed");
        }
        if (poll_result < 0) {
            if (errno == EINTR) {
                fd = fd_.load();
                continue;
            }
            return Result<UdpDatagram>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, errno_message("poll"));
        }
        if (poll_result == 0) {
            fd = fd_.load();
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return Result<UdpDatagram>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, "UDP socket poll error");
        }
        if ((pfd.revents & POLLIN) == 0) {
            fd = fd_.load();
            continue;
        }

        UdpDatagram datagram;
        datagram.data.resize(max_size);
        sockaddr_in remote{};
        socklen_t remote_len = sizeof(remote);
        const ssize_t n = ::recvfrom(fd, datagram.data.data(), datagram.data.size(), 0,
                                     reinterpret_cast<sockaddr*>(&remote), &remote_len);
        if (n < 0) {
            if (errno == EINTR) {
                fd = fd_.load();
                continue;
            }
            return Result<UdpDatagram>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, errno_message("recvfrom"));
        }

        datagram.data.resize(static_cast<size_t>(n));
        char remote_addr[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &remote.sin_addr, remote_addr, sizeof(remote_addr)) != nullptr) {
            datagram.remote_host = remote_addr;
        }
        datagram.remote_port = ntohs(remote.sin_port);
        return Result<UdpDatagram>::ok(std::move(datagram));
    }
    return Result<UdpDatagram>::err(
        ErrorDomain::Network, ErrorCode::NetworkDisconnected, "UDP socket closed");
}

void UdpSocket::close() {
    if (fd_.load() >= 0) {
        const int fd = release();
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

uint16_t UdpSocket::local_port() const {
    const int fd = fd_.load();
    if (fd < 0) {
        return 0;
    }
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

int UdpSocket::release() noexcept {
    return fd_.exchange(-1);
}

}  // namespace streambridge::linux_platform

