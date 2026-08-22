#include <cstdio>
#include <vector>

#include "streambridge/media_types.h"

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

static void test_media_packet_defaults_keep_existing_behavior() {
    MediaPacket packet;
    CHECK(packet.type == MediaType::Unknown, "default media type changed");
    CHECK(packet.codec == CodecId::Unknown, "default codec changed");
    CHECK(packet.h264_format == H264PacketFormat::Unknown,
          "default H.264 format must be explicit unknown");
    CHECK(!packet.is_video(), "default packet should not be video");
    CHECK(!packet.is_audio(), "default packet should not be audio");
    PASS("media packet defaults");
}

static void test_h264_packet_format_is_copyable_metadata() {
    MediaPacket packet;
    packet.type = MediaType::Video;
    packet.codec = CodecId::H264;
    packet.h264_format = H264PacketFormat::AnnexB;
    packet.pts = TimePointUs{1234};
    packet.data = {0x00, 0x00, 0x00, 0x01, 0x65};

    MediaPacket copy = packet;
    CHECK(copy.is_video(), "packet should be video");
    CHECK(copy.codec == CodecId::H264, "codec should be H.264");
    CHECK(copy.h264_format == H264PacketFormat::AnnexB,
          "H.264 format metadata lost during copy");
    CHECK(copy.pts.us == 1234, "PTS metadata lost during copy");
    CHECK(copy.data == packet.data, "packet payload changed during copy");
    PASS("h264 packet format metadata");
}

int main() {
    std::printf("=== media_types unit tests ===\n");
    test_media_packet_defaults_keep_existing_behavior();
    test_h264_packet_format_is_copyable_metadata();
    std::printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
