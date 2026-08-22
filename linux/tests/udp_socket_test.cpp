#include "network/udp_socket.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace streambridge;
using namespace streambridge::linux_platform;

static int g_failures = 0;

#define CHECK(cond, msg)                                      \
    do {                                                      \
        if (cond) {                                           \
            printf("  PASS: %s\n", msg);                      \
        } else {                                              \
            printf("  FAIL: %s\n", msg);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

static void test_loopback_send_receive() {
    printf("[1] UDP loopback send/receive\n");
    UdpSocket receiver;
    auto bound = receiver.bind(0, "127.0.0.1");
    CHECK(bound.is_ok(), "receiver bound to ephemeral port");
    const uint16_t port = receiver.local_port();
    CHECK(port != 0, "local port discovered");

    UdpSocket sender;
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    auto sent = sender.send_to(payload.data(), payload.size(), "127.0.0.1", port);
    CHECK(sent.is_ok() && *sent == payload.size(), "sender sent payload");

    auto datagram = receiver.recv_datagram(1500);
    CHECK(datagram.is_ok(), "receiver got datagram");
    CHECK(datagram->data == payload, "payload roundtrip");
    CHECK(datagram->remote_port != 0, "remote port set");
}

static void test_close_interrupts_blocking_receive() {
    printf("[2] close interrupts blocking recv\n");
    UdpSocket receiver;
    auto bound = receiver.bind(0, "127.0.0.1");
    CHECK(bound.is_ok(), "receiver bound");

    std::atomic<bool> recv_returned{false};
    std::atomic<bool> recv_failed{false};
    std::thread thread([&]() {
        auto result = receiver.recv_datagram(1500);
        recv_failed.store(result.is_err());
        recv_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    receiver.close();
    thread.join();
    CHECK(recv_returned.load(), "recv thread returned after close");
    CHECK(recv_failed.load(), "recv returned error after close");
    CHECK(!receiver.is_open(), "socket closed");
}

static void test_invalid_address_rejected() {
    printf("[3] invalid IPv4 address rejected\n");
    UdpSocket socket;
    const std::vector<uint8_t> payload = {1};
    auto sent = socket.send_to(payload.data(), payload.size(), "localhost", 9);
    CHECK(sent.is_err(), "non-numeric host rejected for now");
}

int main() {
    printf("== Linux UDP socket unit tests ==\n");
    test_loopback_send_receive();
    test_close_interrupts_blocking_receive();
    test_invalid_address_rejected();
    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
