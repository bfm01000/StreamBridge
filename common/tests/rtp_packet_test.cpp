#include "streambridge/rtp_packet.h"

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

static void test_serialize_parse_roundtrip() {
    printf("[1] fixed header roundtrip\n");
    RtpPacket packet;
    packet.header.marker = true;
    packet.header.payload_type = 96;
    packet.header.sequence_number = 0x1234;
    packet.header.timestamp = 0x01020304;
    packet.header.ssrc = 0xAABBCCDD;
    packet.payload = {0x65, 0x88, 0x99};

    auto wire = serialize_rtp_packet(packet);
    CHECK(wire.is_ok(), "serialize ok");
    CHECK(wire->size() == kRtpFixedHeaderSize + 3, "wire size");
    CHECK((*wire)[0] == 0x80, "version byte");
    CHECK((*wire)[1] == 0xE0, "marker + payload type");
    CHECK((*wire)[2] == 0x12 && (*wire)[3] == 0x34, "sequence big endian");
    CHECK((*wire)[4] == 0x01 && (*wire)[7] == 0x04, "timestamp big endian");
    CHECK((*wire)[8] == 0xAA && (*wire)[11] == 0xDD, "ssrc big endian");

    auto parsed = parse_rtp_packet(*wire);
    CHECK(parsed.is_ok(), "parse ok");
    CHECK(parsed->header.marker, "marker parsed");
    CHECK(parsed->header.payload_type == 96, "payload type parsed");
    CHECK(parsed->header.sequence_number == 0x1234, "sequence parsed");
    CHECK(parsed->header.timestamp == 0x01020304, "timestamp parsed");
    CHECK(parsed->header.ssrc == 0xAABBCCDD, "ssrc parsed");
    CHECK(parsed->payload == packet.payload, "payload parsed");
}

static void test_pts_conversion() {
    printf("[2] pts_us -> RTP 90k timestamp\n");
    CHECK(rtp_timestamp_from_pts_us(0) == 0, "zero pts");
    CHECK(rtp_timestamp_from_pts_us(1'000'000) == 90'000, "one second");
    CHECK(rtp_timestamp_from_pts_us(33'333) == 3'000, "30fps frame duration approx");
    CHECK(rtp_timestamp_from_pts_us(1'000'000, 1234) == 91'234, "base timestamp added");
    CHECK(pts_us_delta_from_rtp_timestamp(90'000) == 1'000'000, "rtp delta to us");
}

static void test_reject_unsupported_header_features() {
    printf("[3] unsupported RTP header features rejected\n");
    std::vector<uint8_t> wire(kRtpFixedHeaderSize, 0);
    wire[0] = 0x90;  // version 2 + extension bit
    wire[1] = 96;
    auto parsed = parse_rtp_packet(wire);
    CHECK(parsed.is_err(), "extension rejected");

    RtpPacket packet;
    packet.header.extension = true;
    auto serialized = serialize_rtp_packet(packet);
    CHECK(serialized.is_err(), "serialize extension rejected");
}

int main() {
    printf("== RTP packet unit tests ==\n");
    test_serialize_parse_roundtrip();
    test_pts_conversion();
    test_reject_unsupported_header_features();
    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
