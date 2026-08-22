#include "android_udp_receiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace streambridge::android {
namespace {

std::string errno_message(const char* prefix) {
    std::string message(prefix);
    message += ": ";
    message += std::strerror(errno);
    return message;
}

}  // namespace

AndroidUdpReceiver::~AndroidUdpReceiver() {
    close();
}

Result<void> AndroidUdpReceiver::bind(uint16_t local_port) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return Result<void>::err(
            ErrorDomain::Network, ErrorCode::NetworkConnectFailed, errno_message("socket"));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string message = errno_message("bind");
        close();
        return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed, message);
    }

    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        local_port_ = ntohs(actual.sin_port);
    } else {
        local_port_ = local_port;
    }
    return Result<void>::ok();
}

Result<AndroidUdpDatagram> AndroidUdpReceiver::recv_datagram(size_t max_size) {
    if (!is_open()) {
        return Result<AndroidUdpDatagram>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Android UDP receiver is not open");
    }
    if (max_size == 0) {
        return Result<AndroidUdpDatagram>::err(
            ErrorDomain::Internal, ErrorCode::InvalidArgument, "max datagram size is zero");
    }

    while (is_open()) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int poll_result = ::poll(&pfd, 1, 200);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<AndroidUdpDatagram>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, errno_message("poll"));
        }
        if (poll_result == 0) {
            continue;
        }

        AndroidUdpDatagram datagram;
        datagram.data.resize(max_size);
        const ssize_t received = ::recvfrom(fd_, datagram.data.data(), datagram.data.size(), 0,
                                            nullptr, nullptr);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<AndroidUdpDatagram>::err(
                ErrorDomain::Network, ErrorCode::NetworkReadFailed, errno_message("recvfrom"));
        }
        datagram.data.resize(static_cast<size_t>(received));
        return Result<AndroidUdpDatagram>::ok(std::move(datagram));
    }

    return Result<AndroidUdpDatagram>::err(
        ErrorDomain::Network, ErrorCode::NetworkDisconnected, "Android UDP receiver closed");
}

void AndroidUdpReceiver::close() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    local_port_ = 0;
}

}  // namespace streambridge::android

