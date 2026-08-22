#include "android_udp_sender.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace streambridge::android {
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
            "Android RTP sender expects an IPv4 numeric address");
    }
    return Result<sockaddr_in>::ok(addr);
}

}  // namespace

AndroidUdpSender::~AndroidUdpSender() {
    close();
}

Result<void> AndroidUdpSender::open(uint16_t local_port) {
    if (is_open()) {
        return Result<void>::ok();
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return Result<void>::err(
            ErrorDomain::Network, ErrorCode::NetworkConnectFailed, errno_message("socket"));
    }
    if (local_port != 0) {
        auto addr = make_ipv4_addr("0.0.0.0", local_port);
        if (addr.is_err()) {
            close();
            return Result<void>::err(addr.error_domain(), addr.error_code(), addr.error_message());
        }
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&(*addr)), sizeof(sockaddr_in)) != 0) {
            const std::string message = errno_message("bind");
            close();
            return Result<void>::err(ErrorDomain::Network, ErrorCode::NetworkConnectFailed, message);
        }
    }
    return Result<void>::ok();
}

Result<size_t> AndroidUdpSender::send_to(const uint8_t* data, size_t size,
                                         const std::string& host, uint16_t port) {
    if (data == nullptr || size == 0) {
        return Result<size_t>::err(
            ErrorDomain::Internal, ErrorCode::InvalidArgument, "empty UDP payload");
    }
    if (!is_open()) {
        auto opened = open();
        if (opened.is_err()) {
            return Result<size_t>::err(opened.error_domain(), opened.error_code(), opened.error_message());
        }
    }
    auto addr = make_ipv4_addr(host, port);
    if (addr.is_err()) {
        return Result<size_t>::err(addr.error_domain(), addr.error_code(), addr.error_message());
    }
    const ssize_t sent = ::sendto(fd_, data, size, 0,
                                  reinterpret_cast<const sockaddr*>(&(*addr)),
                                  sizeof(sockaddr_in));
    if (sent < 0) {
        return Result<size_t>::err(
            ErrorDomain::Network, ErrorCode::NetworkWriteFailed, errno_message("sendto"));
    }
    return Result<size_t>::ok(static_cast<size_t>(sent));
}

void AndroidUdpSender::close() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace streambridge::android
