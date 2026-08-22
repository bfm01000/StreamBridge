#include "streambridge/h264_rtp_packetizer.h"

#include <cstdio>
#include <vector>

using namespace streambridge;

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

static MediaPacket make_h264_packet(std::vector<uint8_t> data,
                                    H264PacketFormat format,
                                    int64_t pts_us = 0) {
    MediaPacket packet;
    packet.type = MediaType::Video;
    packet.codec = CodecId::H264;
    packet.h264_format = format;
    packet.pts.us = pts_us;
    packet.data = std::move(data);
    return packet;
}

static void test_single_nal_annexb() {
    printf("[1] Single NAL packetization\n");
    H264RtpPacketizerConfig config;
    config.ssrc = 0x01020304;
    config.payload_type = 97;
    config.initial_sequence_number = 100;
    H264RtpPacketizer packetizer(config);

    auto packet = make_h264_packet({0, 0, 0, 1, 0x65, 0x88, 0x99},
                                  H264PacketFormat::AnnexB,
                                  1'000'000);
    auto packets = packetizer.packetize(packet);
    CHECK(packets.is_ok(), "packetize ok");
    CHECK(packets->size() == 1, "one RTP packet");
    CHECK((*packets)[0].header.marker, "marker on final packet");
    CHECK((*packets)[0].header.payload_type == 97, "payload type");
    CHECK((*packets)[0].header.sequence_number == 100, "sequence");
    CHECK((*packets)[0].header.timestamp == 90'000, "timestamp from pts_us");
    CHECK((*packets)[0].header.ssrc == 0x01020304, "ssrc");
    CHECK((*packets)[0].payload == std::vector<uint8_t>({0x65, 0x88, 0x99}), "payload");
}

static void test_fua_fragmentation() {
    printf("[2] FU-A fragmentation\n");
    H264RtpPacketizerConfig config;
    config.max_payload_size = 6;
    config.initial_sequence_number = 65534;
    H264RtpPacketizer packetizer(config);

    std::vector<uint8_t> annexb = {0, 0, 1, 0x65};
    for (uint8_t i = 0; i < 11; ++i) {
        annexb.push_back(static_cast<uint8_t>(0xA0 + i));
    }
    auto packet = make_h264_packet(std::move(annexb), H264PacketFormat::AnnexB);
    auto packets = packetizer.packetize(packet);
    CHECK(packets.is_ok(), "packetize ok");
    CHECK(packets->size() == 3, "three FU-A packets");
    CHECK((*packets)[0].header.sequence_number == 65534, "first sequence");
    CHECK((*packets)[1].header.sequence_number == 65535, "second sequence");
    CHECK((*packets)[2].header.sequence_number == 0, "sequence wraps");
    CHECK(!(*packets)[0].header.marker && !(*packets)[1].header.marker, "marker off before end");
    CHECK((*packets)[2].header.marker, "marker on FU-A end");
    CHECK((*packets)[0].payload[0] == 0x7C, "FU indicator");
    CHECK(((*packets)[0].payload[1] & 0x80) != 0, "FU start bit");
    CHECK(((*packets)[0].payload[1] & 0x40) == 0, "first FU not end");
    CHECK(((*packets)[2].payload[1] & 0x40) != 0, "FU end bit");
    CHECK(((*packets)[1].payload[1] & 0xC0) == 0, "middle FU has no start/end");
}

static void test_avcc_packetization() {
    printf("[3] AVCC length-prefixed packetization\n");
    H264RtpPacketizer packetizer({});
    std::vector<uint8_t> avcc = {
        0, 0, 0, 2, 0x67, 0x42,
        0, 0, 0, 2, 0x68, 0xCE,
        0, 0, 0, 3, 0x65, 0x01, 0x02,
    };
    auto packet = make_h264_packet(std::move(avcc), H264PacketFormat::AvccLengthPrefixed);
    auto packets = packetizer.packetize(packet);
    CHECK(packets.is_ok(), "packetize ok");
    CHECK(packets->size() == 3, "three single NAL packets");
    CHECK(!(*packets)[0].header.marker && !(*packets)[1].header.marker, "marker before final");
    CHECK((*packets)[2].header.marker, "marker final");
    CHECK((*packets)[0].payload[0] == 0x67, "SPS payload");
    CHECK((*packets)[1].payload[0] == 0x68, "PPS payload");
    CHECK((*packets)[2].payload[0] == 0x65, "IDR payload");
}

static void test_sps_pps_cache_resend_before_idr() {
    printf("[4] SPS/PPS cache resend before IDR\n");
    H264RtpPacketizer packetizer({});

    auto config_packet = make_h264_packet(
        {0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x68, 0xCE},
        H264PacketFormat::AnnexB);
    auto config_packets = packetizer.packetize(config_packet);
    CHECK(config_packets.is_ok(), "config packetize ok");
    CHECK(config_packets->size() == 2, "config emits SPS/PPS");

    auto idr_packet = make_h264_packet({0, 0, 1, 0x65, 0x11, 0x22},
                                      H264PacketFormat::AnnexB);
    auto idr_packets = packetizer.packetize(idr_packet);
    CHECK(idr_packets.is_ok(), "IDR packetize ok");
    CHECK(idr_packets->size() == 3, "SPS/PPS prepended before IDR");
    CHECK((*idr_packets)[0].payload[0] == 0x67, "cached SPS first");
    CHECK((*idr_packets)[1].payload[0] == 0x68, "cached PPS second");
    CHECK((*idr_packets)[2].payload[0] == 0x65, "IDR third");
    CHECK((*idr_packets)[2].header.marker, "marker only final IDR");
}

static void test_reject_unknown_format() {
    printf("[5] unknown format rejected\n");
    H264RtpPacketizer packetizer({});
    auto packet = make_h264_packet({0x12, 0x34, 0x56}, H264PacketFormat::Unknown);
    auto packets = packetizer.packetize(packet);
    CHECK(packets.is_err(), "unknown/malformed H.264 rejected");
}

int main() {
    printf("== H.264 RTP packetizer unit tests ==\n");
    test_single_nal_annexb();
    test_fua_fragmentation();
    test_avcc_packetization();
    test_sps_pps_cache_resend_before_idr();
    test_reject_unknown_format();
    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
