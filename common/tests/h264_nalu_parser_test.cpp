#include <cstdio>
#include <vector>

#include "streambridge/h264_nalu_parser.h"

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

static const uint8_t kSps[] = {0x67, 0x42, 0x00, 0x1f};
static const uint8_t kPps[] = {0x68, 0xce, 0x06, 0xe2};
static const uint8_t kIdr[] = {0x65, 0x88, 0x84};

static void test_annexb_3_and_4_byte_start_codes() {
    std::vector<uint8_t> data = {
        0x00, 0x00, 0x01,
        0x67, 0x42, 0x00, 0x1f,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xce, 0x06, 0xe2,
        0x00, 0x00, 0x01,
        0x65, 0x88, 0x84,
    };
    CHECK(h264_detect_packet_format(data.data(), data.size()) == H264PacketFormat::AnnexB,
          "Annex-B format not detected");
    auto parsed = h264_parse_nalus(data.data(), data.size(), H264PacketFormat::AnnexB);
    CHECK(parsed.ok, parsed.error.c_str());
    CHECK(parsed.nalus.size() == 3, "Annex-B NAL count mismatch");
    CHECK(parsed.nalus[0].is_sps(), "first NAL should be SPS");
    CHECK(parsed.nalus[1].is_pps(), "second NAL should be PPS");
    CHECK(parsed.nalus[2].is_idr(), "third NAL should be IDR");
    PASS("Annex-B parser");
}

static void append_len(std::vector<uint8_t>& out, const uint8_t* data, size_t size,
                       int length_size) {
    for (int i = length_size - 1; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((size >> (i * 8)) & 0xff));
    }
    out.insert(out.end(), data, data + size);
}

static void test_avcc_length_size(int length_size) {
    std::vector<uint8_t> data;
    append_len(data, kSps, sizeof(kSps), length_size);
    append_len(data, kPps, sizeof(kPps), length_size);
    append_len(data, kIdr, sizeof(kIdr), length_size);

    int detected = 0;
    CHECK(h264_detect_packet_format(data.data(), data.size(), &detected) ==
              H264PacketFormat::AvccLengthPrefixed,
          "AVCC format not detected");
    CHECK(detected == length_size, "AVCC length size mismatch");
    auto parsed = h264_parse_nalus(data.data(), data.size(),
                                   H264PacketFormat::AvccLengthPrefixed,
                                   detected);
    CHECK(parsed.ok, parsed.error.c_str());
    CHECK(parsed.nalus.size() == 3, "AVCC NAL count mismatch");
    CHECK(parsed.nalus[0].is_sps(), "first AVCC NAL should be SPS");
    CHECK(parsed.nalus[1].is_pps(), "second AVCC NAL should be PPS");
    CHECK(parsed.nalus[2].is_idr(), "third AVCC NAL should be IDR");
}

static void test_avcc_1_2_4_byte_lengths() {
    test_avcc_length_size(1);
    test_avcc_length_size(2);
    test_avcc_length_size(4);
    PASS("AVCC parser length sizes");
}

static void test_malformed_avcc_rejected() {
    std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x08, 0x65, 0x88};
    int detected = 0;
    CHECK(!h264_detect_avcc_length_size(data.data(), data.size(), &detected),
          "malformed AVCC should not detect length size");
    auto parsed = h264_parse_nalus(data.data(), data.size(),
                                   H264PacketFormat::AvccLengthPrefixed, 4);
    CHECK(!parsed.ok, "malformed AVCC should fail parse");
    PASS("malformed AVCC rejected");
}

int main() {
    std::printf("=== h264_nalu_parser unit tests ===\n");
    test_annexb_3_and_4_byte_start_codes();
    test_avcc_1_2_4_byte_lengths();
    test_malformed_avcc_rejected();
    std::printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}