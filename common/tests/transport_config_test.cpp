#include <cstdio>
#include <string>

#include "streambridge/transport_config.h"

using namespace streambridge;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL: %s\n", msg); \
            ++g_failed; \
            return; \
        } \
    } while (0)

#define PASS(name) \
    do { \
        std::printf("PASS: %s\n", name); \
        ++g_passed; \
    } while (0)

static void test_default_transport_is_rtmp() {
    TransportConfig config;
    CHECK(config.kind == TransportKind::RtmpFlv, "default transport must remain RTMP/FLV");
    CHECK(config.is_rtmp(), "default config should report RTMP");
    CHECK(!config.is_rtp_udp_video(), "default config should not report RTP/UDP");
    CHECK(config.rtmp_flv.connect_timeout_ms == 10'000, "RTMP connect timeout changed");
    PASS("default transport is RTMP");
}

static void test_rtp_udp_video_config_is_explicit() {
    TransportConfig config;
    config.kind = TransportKind::RtpUdpVideo;
    config.rtp_udp_video.remote_host = "127.0.0.1";
    config.rtp_udp_video.remote_port = 5004;
    config.rtp_udp_video.local_port = 5006;
    config.rtp_udp_video.ssrc = 0x11223344;

    CHECK(config.is_rtp_udp_video(), "RTP/UDP selection not active");
    CHECK(!config.is_rtmp(), "RTP/UDP config should not report RTMP");
    CHECK(config.rtp_udp_video.max_payload_size == 1200, "RTP payload default changed");
    CHECK(config.rtp_udp_video.payload_type == 96, "H.264 dynamic payload type changed");
    PASS("explicit RTP/UDP video config");
}

static void test_transport_kind_names_are_stable() {
    CHECK(std::string(transport_kind_name(TransportKind::RtmpFlv)) == "RTMP/FLV",
          "RTMP transport name changed");
    CHECK(std::string(transport_kind_name(TransportKind::RtpUdpVideo)) == "RTP/UDP video",
          "RTP transport name changed");
    PASS("transport kind names");
}

int main() {
    std::printf("=== transport_config unit tests ===\n");
    test_default_transport_is_rtmp();
    test_rtp_udp_video_config_is_explicit();
    test_transport_kind_names_are_stable();
    std::printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}