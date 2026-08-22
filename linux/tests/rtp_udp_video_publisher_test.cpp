#include "rtp_udp_video_publisher.h"

#include "network/udp_socket.h"
#include "streambridge/h264_rtp_depacketizer.h"
#include "streambridge/rtp_packet.h"

#include <cstdio>
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

static void test_publish_h264_to_udp_loopback() {
    printf("[1] RTP/UDP publisher loopback\n");
    UdpSocket receiver;
    auto bound = receiver.bind(0, "127.0.0.1");
    CHECK(bound.is_ok(), "receiver bound");

    RtpUdpVideoTransportConfig config;
    config.remote_host = "127.0.0.1";
    config.remote_port = receiver.local_port();
    config.payload_type = 96;
    config.ssrc = 0x11223344;
    config.max_payload_size = 1200;

    RtpUdpVideoPublisher publisher(config);
    auto opened = publisher.open({});
    CHECK(opened.is_ok(), "publisher opened");

    StreamInfo video;
    video.type = MediaType::Video;
    video.codec = CodecId::H264;
    auto header = publisher.write_header(video, {});
    CHECK(header.is_ok(), "header accepted");

    const std::vector<uint8_t> annexb = {0, 0, 0, 1, 0x65, 0xAA, 0xBB};
    auto wrote = publisher.write_packet(make_h264_packet(annexb, 1'000'000));
    CHECK(wrote.is_ok(), "publisher wrote packet");
    CHECK(publisher.stats().packets_written == 1, "one RTP packet sent");
    CHECK(publisher.stats().bytes_written == 15, "wire bytes counted");

    auto datagram = receiver.recv_datagram(1500);
    CHECK(datagram.is_ok(), "receiver got UDP datagram");
    auto parsed = parse_rtp_packet(datagram->data);
    CHECK(parsed.is_ok(), "RTP parsed");
    CHECK(parsed->header.ssrc == 0x11223344, "SSRC preserved");
    CHECK(parsed->header.timestamp == 90'000, "timestamp preserved");

    H264RtpDepacketizer depacketizer;
    auto frames = depacketizer.push_packet(*parsed);
    CHECK(frames.is_ok(), "depacketizer accepted RTP packet");
    CHECK(frames->size() == 1, "one frame restored");
    CHECK((*frames)[0].data == annexb, "Annex-B restored");
}

static void test_audio_header_rejected() {
    printf("[2] audio header rejected for current RTP stage\n");
    RtpUdpVideoTransportConfig config;
    config.remote_host = "127.0.0.1";
    config.remote_port = 9;
    RtpUdpVideoPublisher publisher(config);
    auto opened = publisher.open({});
    CHECK(opened.is_ok(), "publisher opened");

    StreamInfo video;
    video.type = MediaType::Video;
    video.codec = CodecId::H264;
    StreamInfo audio;
    audio.type = MediaType::Audio;
    audio.codec = CodecId::AAC;
    auto header = publisher.write_header(video, audio);
    CHECK(header.is_err(), "audio rejected");
}

int main() {
    printf("== RTP/UDP video publisher unit tests ==\n");
    test_publish_h264_to_udp_loopback();
    test_audio_header_rejected();
    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
