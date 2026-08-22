#include "network/rtp_udp_video_receiver.h"
#include "rtp_udp_video_publisher.h"

#include <cstdio>
#include <utility>
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

static MediaPacket make_h264_packet(std::vector<uint8_t> data, int64_t pts_us = 0) {
    MediaPacket packet;
    packet.type = MediaType::Video;
    packet.codec = CodecId::H264;
    packet.h264_format = H264PacketFormat::AnnexB;
    packet.pts.us = pts_us;
    packet.data = std::move(data);
    return packet;
}

static void test_publisher_to_receiver_loopback() {
    printf("[1] RTP publisher -> receiver loopback\n");
    RtpUdpVideoReceiver receiver;
    RtpUdpVideoReceiverConfig receiver_config;
    receiver_config.local_port = 0;
    auto opened_receiver = receiver.open(receiver_config);
    CHECK(opened_receiver.is_ok(), "receiver opened");

    RtpUdpVideoTransportConfig publisher_config;
    publisher_config.remote_host = "127.0.0.1";
    publisher_config.remote_port = receiver.local_port();
    publisher_config.ssrc = 0x55667788;
    publisher_config.max_payload_size = 6;
    RtpUdpVideoPublisher publisher(publisher_config);
    auto opened_publisher = publisher.open({});
    CHECK(opened_publisher.is_ok(), "publisher opened");

    StreamInfo video;
    video.type = MediaType::Video;
    video.codec = CodecId::H264;
    auto header = publisher.write_header(video, {});
    CHECK(header.is_ok(), "publisher header ok");

    std::vector<uint8_t> annexb = {0, 0, 0, 1, 0x65};
    for (uint8_t i = 0; i < 11; ++i) {
        annexb.push_back(static_cast<uint8_t>(0xD0 + i));
    }
    auto wrote = publisher.write_packet(make_h264_packet(annexb, 2'000'000));
    CHECK(wrote.is_ok(), "publisher wrote fragmented frame");
    CHECK(publisher.stats().packets_written == 3, "FU-A packets sent");

    auto frame = receiver.read_frame();
    CHECK(frame.is_ok(), "receiver read frame");
    CHECK(frame->data == annexb, "receiver restored Annex-B frame");
    CHECK(frame->pts.us == 0, "first received frame pts normalized");
    CHECK(receiver.stats().udp_datagrams == 3, "receiver counted UDP datagrams");
    CHECK(receiver.stats().depacketizer.output_frames == 1, "receiver depacketizer output frame");
}

int main() {
    printf("== RTP/UDP video receiver pipeline tests ==\n");
    test_publisher_to_receiver_loopback();
    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}

