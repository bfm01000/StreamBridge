#include "streambridge/h264_rtp_depacketizer.h"
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

static MediaPacket make_annexb_packet(std::vector<uint8_t> data, int64_t pts_us = 0) {
    MediaPacket packet;
    packet.type = MediaType::Video;
    packet.codec = CodecId::H264;
    packet.h264_format = H264PacketFormat::AnnexB;
    packet.pts.us = pts_us;
    packet.data = std::move(data);
    return packet;
}

static void test_packetizer_depacketizer_roundtrip() {
    printf("[1] packetizer/depacketizer roundtrip\n");
    H264RtpPacketizerConfig packetizer_config;
    packetizer_config.repeat_parameter_sets_before_idr = false;
    H264RtpPacketizer packetizer(packetizer_config);
    H264RtpDepacketizer depacketizer;

    const std::vector<uint8_t> annexb = {
        0, 0, 0, 1, 0x67, 0x42,
        0, 0, 1, 0x68, 0xCE,
        0, 0, 1, 0x65, 0x11, 0x22,
    };
    const std::vector<uint8_t> normalized_annexb = {
        0, 0, 0, 1, 0x67, 0x42,
        0, 0, 0, 1, 0x68, 0xCE,
        0, 0, 0, 1, 0x65, 0x11, 0x22,
    };
    auto rtp_packets = packetizer.packetize(make_annexb_packet(annexb, 33'333));
    CHECK(rtp_packets.is_ok(), "packetize ok");

    std::vector<MediaPacket> frames;
    for (const auto& rtp : *rtp_packets) {
        auto out = depacketizer.push_packet(rtp);
        CHECK(out.is_ok(), "depacketize packet ok");
        frames.insert(frames.end(), out->begin(), out->end());
    }

    CHECK(frames.size() == 1, "one frame output");
    CHECK(frames[0].h264_format == H264PacketFormat::AnnexB, "output Annex-B");
    CHECK(frames[0].pts.us == 0, "first RTP timestamp normalized to zero pts");
    CHECK(frames[0].data == normalized_annexb, "Annex-B access unit restored");
    CHECK(frames[0].is_key_frame, "IDR marks key frame");
}

static void test_small_reorder_window() {
    printf("[2] small reorder window\n");
    H264RtpDepacketizerConfig config;
    config.max_reorder_packets = 4;
    H264RtpDepacketizer depacketizer(config);

    RtpPacket p0;
    p0.header.sequence_number = 10;
    p0.header.timestamp = 90'000;
    p0.header.payload_type = 96;
    p0.payload = {0x61, 0x01};

    RtpPacket p1 = p0;
    p1.header.sequence_number = 11;
    p1.payload = {0x61, 0x02};

    RtpPacket p2 = p0;
    p2.header.sequence_number = 12;
    p2.header.marker = true;
    p2.payload = {0x61, 0x03};

    auto out0 = depacketizer.push_packet(p0);
    auto out2 = depacketizer.push_packet(p2);
    auto out1 = depacketizer.push_packet(p1);
    CHECK(out0.is_ok() && out2.is_ok() && out1.is_ok(), "push packets ok");
    CHECK(out0->empty() && out2->empty(), "no early frame before missing packet arrives");
    CHECK(out1->size() == 1, "frame emitted after reorder completes");
    CHECK(depacketizer.stats().reordered_packets >= 1, "reordered packet counted");
}

static void test_fua_loss_drops_frame() {
    printf("[3] FU-A loss drops damaged frame\n");
    H264RtpPacketizerConfig packetizer_config;
    packetizer_config.max_payload_size = 6;
    packetizer_config.initial_sequence_number = 20;
    H264RtpPacketizer packetizer(packetizer_config);

    std::vector<uint8_t> annexb = {0, 0, 1, 0x65};
    for (uint8_t i = 0; i < 11; ++i) {
        annexb.push_back(static_cast<uint8_t>(0xC0 + i));
    }
    auto rtp_packets = packetizer.packetize(make_annexb_packet(std::move(annexb)));
    CHECK(rtp_packets.is_ok() && rtp_packets->size() == 3, "large NAL fragmented");

    H264RtpDepacketizerConfig depacketizer_config;
    depacketizer_config.max_reorder_packets = 0;
    H264RtpDepacketizer depacketizer(depacketizer_config);
    auto out0 = depacketizer.push_packet((*rtp_packets)[0]);
    auto out2 = depacketizer.push_packet((*rtp_packets)[2]);
    CHECK(out0.is_ok() && out2.is_ok(), "push start and end with middle missing");
    CHECK(out0->empty() && out2->empty(), "no damaged frame output");
    CHECK(depacketizer.stats().lost_packets == 1, "lost packet counted");
    CHECK(depacketizer.stats().dropped_frames == 1, "damaged FU-A frame dropped");
}

static void test_malformed_packet_rejected() {
    printf("[4] malformed packet rejected\n");
    H264RtpDepacketizer depacketizer;
    RtpPacket packet;
    packet.header.payload_type = 97;
    packet.payload = {0x65};
    auto out = depacketizer.push_packet(packet);
    CHECK(out.is_err(), "wrong payload type rejected");

    RtpPacket fua;
    fua.header.payload_type = 96;
    fua.payload = {0x7C, 0x85};
    auto out2 = depacketizer.push_packet(fua);
    CHECK(out2.is_err(), "short FU-A rejected");
}

int main() {
    printf("== H.264 RTP depacketizer unit tests ==\n");
    test_packetizer_depacketizer_roundtrip();
    test_small_reorder_window();
    test_fua_loss_drops_frame();
    test_malformed_packet_rejected();
    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}

