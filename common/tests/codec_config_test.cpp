// Unit tests for codec_config parser
// Build: g++ -std=c++17 -I../include -I../src -o codec_config_test \
//        codec_config_test.cpp ../src/ffmpeg/codec_config.cpp \
//        $(pkg-config --cflags libavcodec) $(pkg-config --libs libavcodec)
// Run: ./codec_config_test

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "ffmpeg/codec_config.h"
#include "streambridge/media_errors.h"

using namespace streambridge::ffmpeg;

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) \
    do { printf("  %s ... ", name); } while(0)

#define PASS() \
    do { printf("PASS\n"); g_passed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); g_failed++; } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

#define CHECK_ERR(result, expected_code, msg) \
    do { \
        if (!(result).is_err()) { FAIL(msg " (expected err)"); return; } \
        if ((result).error_code() != (expected_code)) { \
            printf("FAIL: %s (got code %d)\n", msg, static_cast<int>((result).error_code())); \
            g_failed++; return; \
        } \
    } while(0)

// ============================================================
// Helpers: build test extradata buffers
// ============================================================

// Minimal avcC with 1 SPS + 1 PPS
static std::vector<uint8_t> make_avcc(
        const uint8_t* sps, size_t sps_len,
        const uint8_t* pps, size_t pps_len,
        int length_size_minus_one = 3) {
    std::vector<uint8_t> buf;
    buf.push_back(1);  // configurationVersion
    buf.push_back(0x64); // AVCProfileIndication (High)
    buf.push_back(0x00); // profile_compatibility
    buf.push_back(0x1F); // AVCLevelIndication (3.1)
    buf.push_back(static_cast<uint8_t>(0xFC | (length_size_minus_one & 0x03)));
    buf.push_back(static_cast<uint8_t>(0xE0 | 1)); // 1 SPS
    buf.push_back(static_cast<uint8_t>((sps_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(sps_len & 0xFF));
    buf.insert(buf.end(), sps, sps + sps_len);
    buf.push_back(1); // 1 PPS
    buf.push_back(static_cast<uint8_t>((pps_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(pps_len & 0xFF));
    buf.insert(buf.end(), pps, pps + pps_len);
    return buf;
}

// Minimal hvcC with VPS + SPS + PPS
static std::vector<uint8_t> make_hvcc(
        const uint8_t* vps, size_t vps_len,
        const uint8_t* sps, size_t sps_len,
        const uint8_t* pps, size_t pps_len) {
    std::vector<uint8_t> buf;
    buf.push_back(1); // configurationVersion
    // bytes 1-12: general profile/level/compat info (simplified)
    buf.push_back(0x01); buf.push_back(0x00); buf.push_back(0x60);
    buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);
    buf.push_back(0x90); buf.push_back(0x00); buf.push_back(0x00);
    buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);
    // byte 13-20: more flags, min_spatial_seg, parallelism, chroma
    for (int i = 0; i < 8; i++) buf.push_back(0x00);
    // byte 21: 0xFC | lengthSizeMinusOne(3)
    buf.push_back(0xFF);
    // byte 22: numArrays = 3 (VPS/SPS/PPS)
    buf.push_back(3);

    // Helper: add array entry
    auto add_array = [&](int nal_type, const uint8_t* data, size_t len) {
        buf.push_back(static_cast<uint8_t>(nal_type & 0x3F)); // completeness=0
        buf.push_back(0x00); // numNalus high
        buf.push_back(0x01); // numNalus = 1
        buf.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(len & 0xFF));
        buf.insert(buf.end(), data, data + len);
    };

    add_array(32, vps, vps_len);  // VPS
    add_array(33, sps, sps_len);  // SPS
    add_array(34, pps, pps_len);  // PPS

    return buf;
}

// ============================================================
// Test data
// ============================================================

// Minimal SPS (baseline, 320x240)
static const uint8_t kSps[] = {0x67, 0x42, 0x00, 0x0A, 0xA6, 0x02, 0x80, 0xBF, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x44, 0x00, 0x00, 0x0F, 0x42, 0xE0};

// Minimal PPS
static const uint8_t kPps[] = {0x68, 0xC8, 0x42, 0x0F};

// Minimal VPS (H.265)
static const uint8_t kVps[] = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x99, 0x20, 0x02, 0x40};

// Minimal SPS (H.265)
static const uint8_t kSpsHevc[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x99, 0xA0, 0x02, 0x80, 0x80, 0x2D, 0x13, 0x65, 0x93, 0x2C, 0x80, 0xCA, 0x1F, 0xAC, 0x12, 0x12, 0x12, 0x24};

// Minimal PPS (H.265)
static const uint8_t kPpsHevc[] = {0x44, 0x01, 0xC0, 0x25, 0x2F, 0x05, 0x32, 0x40};

// ============================================================
// Tests
// ============================================================

static void test_avcc_standard() {
    TEST("avcc standard (1 SPS + 1 PPS)");
    auto buf = make_avcc(kSps, sizeof(kSps), kPps, sizeof(kPps));
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    auto& cfg = *result;
    CHECK(cfg.format == BitstreamFormat::Avcc, "format not avcC");
    CHECK(cfg.sps_list.size() == 1, "sps count != 1");
    CHECK(cfg.pps_list.size() == 1, "pps count != 1");
    CHECK(cfg.nal_length_size == 4, "nal_size != 4");
    CHECK(cfg.is_complete(), "not complete");
    CHECK(cfg.sps_list[0].data.size() == sizeof(kSps), "sps size mismatch");
    CHECK(memcmp(cfg.sps_list[0].data.data(), kSps, sizeof(kSps)) == 0, "sps data mismatch");
    CHECK(cfg.pps_list[0].data.size() == sizeof(kPps), "pps size mismatch");
    CHECK(memcmp(cfg.pps_list[0].data.data(), kPps, sizeof(kPps)) == 0, "pps data mismatch");
    PASS();
}

static void test_avcc_multi_sps_pps() {
    TEST("avcc multi SPS/PPS (2+2)");
    std::vector<uint8_t> buf;
    buf.push_back(1); buf.push_back(0x64); buf.push_back(0x00); buf.push_back(0x1F);
    buf.push_back(0xFF); // lengthSizeMinusOne=3
    buf.push_back(0xE0 | 2); // 2 SPS
    for (int i = 0; i < 2; i++) {
        buf.push_back(0x00); buf.push_back(static_cast<uint8_t>(sizeof(kSps)));
        buf.insert(buf.end(), kSps, kSps + sizeof(kSps));
    }
    buf.push_back(2); // 2 PPS
    for (int i = 0; i < 2; i++) {
        buf.push_back(0x00); buf.push_back(static_cast<uint8_t>(sizeof(kPps)));
        buf.insert(buf.end(), kPps, kPps + sizeof(kPps));
    }
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->sps_list.size() == 2, "sps != 2");
    CHECK(result->pps_list.size() == 2, "pps != 2");
    CHECK(result->is_complete(), "not complete");
    PASS();
}

static void test_avcc_nal_size_1() {
    TEST("avcc nal_length_size=1");
    auto buf = make_avcc(kSps, sizeof(kSps), kPps, sizeof(kPps), 0);
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->nal_length_size == 1, "nal_size != 1");
    PASS();
}

static void test_avcc_nal_size_4() {
    TEST("avcc nal_length_size=4");
    auto buf = make_avcc(kSps, sizeof(kSps), kPps, sizeof(kPps), 3);
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->nal_length_size == 4, "nal_size != 4");
    PASS();
}

static void test_annexb_h264_4byte() {
    TEST("Annex-B H.264 4-byte start code");
    std::vector<uint8_t> buf;
    // 00 00 00 01 + SPS + 00 00 00 01 + PPS
    buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01});
    buf.insert(buf.end(), kSps, kSps + sizeof(kSps));
    buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01});
    buf.insert(buf.end(), kPps, kPps + sizeof(kPps));
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->format == BitstreamFormat::AnnexB, "format not AnnexB");
    CHECK(result->sps_list.size() == 1, "sps != 1");
    CHECK(result->pps_list.size() == 1, "pps != 1");
    CHECK(result->is_complete(), "not complete");
    PASS();
}

static void test_annexb_h264_3byte() {
    TEST("Annex-B H.264 3-byte start code");
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), {0x00, 0x00, 0x01});
    buf.insert(buf.end(), kSps, kSps + sizeof(kSps));
    buf.insert(buf.end(), {0x00, 0x00, 0x01});
    buf.insert(buf.end(), kPps, kPps + sizeof(kPps));
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->sps_list.size() == 1, "sps != 1");
    CHECK(result->pps_list.size() == 1, "pps != 1");
    PASS();
}

static void test_hvcc_standard() {
    TEST("hvcC standard (VPS+SPS+PPS)");
    auto buf = make_hvcc(kVps, sizeof(kVps), kSpsHevc, sizeof(kSpsHevc), kPpsHevc, sizeof(kPpsHevc));
    auto result = parse_codec_config(AV_CODEC_ID_H265, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->format == BitstreamFormat::Hvcc, "format not hvcC");
    CHECK(result->vps_list.size() == 1, "vps != 1");
    CHECK(result->sps_list.size() == 1, "sps != 1");
    CHECK(result->pps_list.size() == 1, "pps != 1");
    CHECK(result->is_complete(), "not complete");
    PASS();
}

static void test_annexb_h265() {
    TEST("Annex-B H.265");
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01});
    buf.insert(buf.end(), kVps, kVps + sizeof(kVps));
    buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01});
    buf.insert(buf.end(), kSpsHevc, kSpsHevc + sizeof(kSpsHevc));
    buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01});
    buf.insert(buf.end(), kPpsHevc, kPpsHevc + sizeof(kPpsHevc));
    auto result = parse_codec_config(AV_CODEC_ID_H265, buf.data(), buf.size());
    CHECK(result.is_ok(), "parse failed");
    CHECK(result->format == BitstreamFormat::AnnexB, "format not AnnexB");
    CHECK(result->vps_list.size() == 1, "vps != 1");
    CHECK(result->sps_list.size() == 1, "sps != 1");
    CHECK(result->pps_list.size() == 1, "pps != 1");
    PASS();
}

static void test_truncated_avcc() {
    TEST("truncated avcC");
    // Only 3 bytes — not enough for header
    uint8_t buf[] = {0x01, 0x64, 0x00};
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf, sizeof(buf));
    CHECK_ERR(result, ErrorCode::MalformedAvcc, "should be MalformedAvcc");
    PASS();
}

static void test_truncated_hvcc() {
    TEST("truncated hvcC");
    uint8_t buf[] = {0x01, 0x00};
    auto result = parse_codec_config(AV_CODEC_ID_H265, buf, sizeof(buf));
    CHECK_ERR(result, ErrorCode::MalformedHvcc, "should be MalformedHvcc");
    PASS();
}

static void test_empty_extradata() {
    TEST("empty extradata");
    auto result = parse_codec_config(AV_CODEC_ID_H264, nullptr, 0);
    CHECK(result.is_ok(), "null should return ok(empty)");
    CHECK(!result->is_complete(), "empty should be incomplete");
    CHECK(result->sps_list.empty(), "sps should be empty");
    PASS();
}

static void test_unknown_format() {
    TEST("unknown format (random data)");
    uint8_t buf[] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA};
    auto result = parse_codec_config(AV_CODEC_ID_H264, buf, sizeof(buf));
    CHECK(result.is_err(), "should be err");
    PASS();
}

static void test_no_extradata_from_packet() {
    TEST("bare Annex-B: extract from packets");
    // First packet: SPS only
    std::vector<uint8_t> pkt1;
    pkt1.insert(pkt1.end(), {0x00, 0x00, 0x00, 0x01});
    pkt1.insert(pkt1.end(), kSps, kSps + sizeof(kSps));
    auto r1 = parse_codec_config_from_packet(AV_CODEC_ID_H264, pkt1.data(), pkt1.size());
    CHECK(r1.is_ok(), "pkt1 parse failed");
    CHECK(!r1->is_complete(), "SPS-only should be incomplete");
    CHECK(r1->sps_list.size() == 1, "sps != 1");
    CHECK(r1->pps_list.empty(), "pps should be empty");

    // Second packet: PPS only
    std::vector<uint8_t> pkt2;
    pkt2.insert(pkt2.end(), {0x00, 0x00, 0x00, 0x01});
    pkt2.insert(pkt2.end(), kPps, kPps + sizeof(kPps));
    auto r2 = parse_codec_config_from_packet(AV_CODEC_ID_H264, pkt2.data(), pkt2.size());
    CHECK(r2.is_ok(), "pkt2 parse failed");
    CHECK(r2->pps_list.size() == 1, "pps != 1");

    // Merge
    CodecConfig accumulated;
    accumulated.codec_id = AV_CODEC_ID_H264;
    merge_codec_config(accumulated, *r1);
    CHECK(!accumulated.is_complete(), "should be incomplete before PPS");
    merge_codec_config(accumulated, *r2);
    CHECK(accumulated.is_complete(), "should be complete after PPS");
    CHECK(accumulated.sps_list.size() == 1, "merged sps != 1");
    CHECK(accumulated.pps_list.size() == 1, "merged pps != 1");
    PASS();
}

static void test_detect_format_avcc() {
    TEST("detect_format: avcC");
    uint8_t buf[] = {0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE1};
    auto fmt = detect_bitstream_format(AV_CODEC_ID_H264, buf, sizeof(buf));
    CHECK(fmt == BitstreamFormat::Avcc, "should be avcC");
    PASS();
}

static void test_detect_format_hvcc() {
    TEST("detect_format: hvcC");
    uint8_t buf[] = {0x01, 0x01, 0x60, 0x00, 0x00, 0x00};
    auto fmt = detect_bitstream_format(AV_CODEC_ID_H265, buf, sizeof(buf));
    CHECK(fmt == BitstreamFormat::Hvcc, "should be hvcC");
    PASS();
}

static void test_detect_format_annexb() {
    TEST("detect_format: AnnexB");
    uint8_t buf[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42};
    auto fmt = detect_bitstream_format(AV_CODEC_ID_H264, buf, sizeof(buf));
    CHECK(fmt == BitstreamFormat::AnnexB, "should be AnnexB");
    PASS();
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== codec_config unit tests ===\n\n");

    test_avcc_standard();
    test_avcc_multi_sps_pps();
    test_avcc_nal_size_1();
    test_avcc_nal_size_4();
    test_annexb_h264_4byte();
    test_annexb_h264_3byte();
    test_hvcc_standard();
    test_annexb_h265();
    test_truncated_avcc();
    test_truncated_hvcc();
    test_empty_extradata();
    test_unknown_format();
    test_no_extradata_from_packet();
    test_detect_format_avcc();
    test_detect_format_hvcc();
    test_detect_format_annexb();

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
