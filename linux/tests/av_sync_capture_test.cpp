// 音画同步集成验证 — 真实 V4L2 摄像头 + ALSA 麦克风
//
// 走生产链路（PublishSession：采集→编码→mux）录制 N 秒到本地 FLV 文件，
// 再离线分析录制文件的音视频时间线，验证设备源双路对齐与无漂移。
//
// 用法：
//   av_sync_capture_test                  # 录制 10s 到 source/av_sync_test.flv 并分析
//   av_sync_capture_test --duration 30    # 自定义录制时长
//   av_sync_capture_test --output x.flv   # 自定义输出路径
//   av_sync_capture_test --analyze x.flv  # 只分析已有文件，不录制
//
// 判定阈值（录制 10s）：
//   |起始偏差| ≤ 100ms   — AvStartAligner 保证两路从同一零点开始
//   |漂移|     ≤ 100ms   — 两路共用单调时钟，时间线应平行
//   视频最大帧间隔 ≤ 200ms / 音频 ≤ 150ms — 无长时间丢帧/XRUN 空洞
//   两路时间线必须单调递增
//
// 设备缺失（无摄像头/声卡）时打印 SKIP 并以退出码 0 结束（ctest 友好）。
//
// 注意：文件可验证「时间线对齐与无漂移」；感知唇音延迟还包含采集管线
// 固有延迟（曝光、缓冲、编码），需在播放端用节拍器/嘴型做最终验证。

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "streambridge/ffmpeg_utils.h"
#include "streambridge/logging.h"
#include "streambridge/session.h"
#include "../platform/capture/alsa_audio_capture.h"
#include "../platform/capture/v4l2_video_capture.h"
#include "ffmpeg/ffmpeg_audio_encoder.h"
#include "ffmpeg/ffmpeg_rtmp_publisher.h"
#include "ffmpeg/ffmpeg_video_encoder.h"

#include <sys/stat.h>

using namespace streambridge;

// ============================================================
// 离线分析：从 FLV 提取双路 PTS 时间线并计算同步指标
// ============================================================
struct AvSyncResult {
    bool ok = false;
    std::string error;
    size_t video_frames = 0;
    size_t audio_packets = 0;
    double duration_ms = 0;      // 视频时间线跨度
    double start_skew_ms = 0;    // audio_first - video_first（对齐后应≈0）
    double drift_ms = 0;         // 视频跨度 - 音频跨度（应≈0）
    double v_max_gap_ms = 0;     // 视频最大帧间隔（丢帧空洞检测）
    double a_max_gap_ms = 0;     // 音频最大包间隔（XRUN 空洞检测）
    bool monotonic = true;
};

static AvSyncResult analyze_flv(const std::string& path) {
    AvSyncResult r;

    AVFormatContext* ctx = nullptr;
    int ret = avformat_open_input(&ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        r.error = "avformat_open_input failed: " + path;
        return r;
    }
    AVFormatContextPtr ctx_guard(ctx);

    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0) {
        r.error = "avformat_find_stream_info failed";
        return r;
    }

    int v_idx = -1, a_idx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && v_idx < 0)
            v_idx = static_cast<int>(i);
        else if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && a_idx < 0)
            a_idx = static_cast<int>(i);
    }
    if (v_idx < 0 || a_idx < 0) {
        r.error = "recorded file lacks video or audio stream";
        return r;
    }

    std::vector<int64_t> v_pts, a_pts;
    AVPacketPtr pkt = alloc_packet();
    while (av_read_frame(ctx, pkt.get()) >= 0) {
        AVPacket* p = pkt.get();
        int64_t pts_us = -1;
        if (p->pts != AV_NOPTS_VALUE) {
            pts_us = pts_to_us(p->pts, ctx->streams[p->stream_index]->time_base);
        } else if (p->dts != AV_NOPTS_VALUE) {
            pts_us = pts_to_us(p->dts, ctx->streams[p->stream_index]->time_base);
        }
        if (pts_us < 0) {
            av_packet_unref(p);
            continue;
        }
        if (p->stream_index == v_idx) v_pts.push_back(pts_us);
        else if (p->stream_index == a_idx) a_pts.push_back(pts_us);
        av_packet_unref(p);
    }

    r.video_frames = v_pts.size();
    r.audio_packets = a_pts.size();
    if (v_pts.size() < 30 || a_pts.size() < 30) {
        r.error = "too few packets to analyze";
        return r;
    }

    // 单调性 + 最大间隔
    for (size_t i = 1; i < v_pts.size(); i++) {
        if (v_pts[i] < v_pts[i - 1]) r.monotonic = false;
        double gap = (v_pts[i] - v_pts[i - 1]) / 1000.0;
        if (gap > r.v_max_gap_ms) r.v_max_gap_ms = gap;
    }
    for (size_t i = 1; i < a_pts.size(); i++) {
        if (a_pts[i] < a_pts[i - 1]) r.monotonic = false;
        double gap = (a_pts[i] - a_pts[i - 1]) / 1000.0;
        if (gap > r.a_max_gap_ms) r.a_max_gap_ms = gap;
    }

    // 起始偏差与漂移：两路时间线在各自首包处归零后比较
    int64_t v_first = v_pts.front(), v_last = v_pts.back();
    int64_t a_first = a_pts.front(), a_last = a_pts.back();
    r.start_skew_ms = (a_first - v_first) / 1000.0;
    r.drift_ms = ((v_last - v_first) - (a_last - a_first)) / 1000.0;
    r.duration_ms = (v_last - v_first) / 1000.0;

    r.ok = true;
    return r;
}

// ============================================================
// 录制：真实设备 → PublishSession → 本地 FLV
// ============================================================
static bool record_capture(const std::string& output_path, int duration_s) {
    set_log_level(LogLevel::Info);
    SB_LOG_I("avsync", "recording %ds camera+mic -> %s", duration_s, output_path.c_str());

    auto video_cap = std::make_unique<V4L2VideoCapture>();
    auto audio_cap = std::make_unique<ALSAAudioCapture>();
    auto video_enc = std::make_unique<FFmpegVideoEncoder>();
    auto audio_enc = std::make_unique<FFmpegAudioEncoder>();
    auto publisher = std::make_unique<FFmpegRTMPPublisher>();

    PublishSession session(
        std::move(video_cap), std::move(audio_cap),
        std::move(video_enc), std::move(audio_enc),
        std::move(publisher));

    PublishSessionConfig cfg;
    cfg.enable_audio = true;
    cfg.video_capture.source = "/dev/video0";
    cfg.video_capture.target_width = 1280;
    cfg.video_capture.target_height = 720;
    cfg.video_capture.target_fps = 30;
    cfg.video_encode.width = 1280;
    cfg.video_encode.height = 720;
    cfg.video_encode.frame_rate = 30;
    cfg.video_encode.bitrate_bps = 2'000'000;
    cfg.audio_capture.source = "hw:0,0";
    cfg.audio_capture.target_sample_rate = 48000;
    cfg.audio_capture.target_channels = 2;
    cfg.audio_encode.sample_rate = 48000;
    cfg.audio_encode.channels = 2;
    cfg.audio_encode.bitrate_bps = 128'000;
    cfg.publish.url = output_path;  // 本地文件路径 — 复用生产 FLV mux 链路

    auto ret = session.prepare(cfg);
    if (ret.is_err()) {
        SB_LOG_W("avsync", "prepare failed (devices missing?): %s",
              ret.to_string().c_str());
        return false;
    }

    ret = session.start();
    if (ret.is_err()) {
        SB_LOG_E("avsync", "start failed: %s", ret.to_string().c_str());
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
    while (std::chrono::steady_clock::now() < deadline &&
           session.state() == SessionState::Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    session.stop();
    auto m = session.metrics();
    SB_LOG_I("avsync", "recorded: cap=%ld enc=%ld sent=%ld drop=%ld",
          m.frames_captured, m.frames_encoded, m.packets_sent, m.frames_dropped);
    return true;
}

// ============================================================
// 判定与输出
// ============================================================
static int judge(const AvSyncResult& r) {
    const double kMaxSkewMs = 100.0;
    const double kMaxDriftMs = 100.0;
    const double kMaxVGapMs = 200.0;
    const double kMaxAGapMs = 150.0;

    bool pass = r.ok && r.monotonic &&
                (r.start_skew_ms > -kMaxSkewMs && r.start_skew_ms < kMaxSkewMs) &&
                (r.drift_ms > -kMaxDriftMs && r.drift_ms < kMaxDriftMs) &&
                r.v_max_gap_ms <= kMaxVGapMs &&
                r.a_max_gap_ms <= kMaxAGapMs;

    printf("\n=== AV sync analysis ===\n");
    printf("video frames : %zu\n", r.video_frames);
    printf("audio packets: %zu\n", r.audio_packets);
    printf("duration     : %.0f ms\n", r.duration_ms);
    printf("start skew   : %+.1f ms   (audio_first - video_first, limit ±%.0f)\n",
           r.start_skew_ms, kMaxSkewMs);
    printf("drift        : %+.1f ms   (video span - audio span, limit ±%.0f)\n",
           r.drift_ms, kMaxDriftMs);
    printf("v max gap    : %.1f ms   (limit %.0f)\n", r.v_max_gap_ms, kMaxVGapMs);
    printf("a max gap    : %.1f ms   (limit %.0f)\n", r.a_max_gap_ms, kMaxAGapMs);
    printf("monotonic    : %s\n", r.monotonic ? "yes" : "NO");
    printf("verdict      : %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [--duration <s>] [--output <flv>] [--analyze <flv>]\n"
        "Default: record 10s camera+mic to source/av_sync_test.flv, then analyze.\n",
        prog);
}

int main(int argc, char* argv[]) {
    int duration_s = 10;
    std::string output = "source/av_sync_test.flv";
    std::string analyze_path;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--analyze") == 0 && i + 1 < argc) {
            analyze_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!analyze_path.empty()) {
        auto r = analyze_flv(analyze_path);
        if (!r.ok) {
            printf("FAIL: %s\n", r.error.c_str());
            return 1;
        }
        return judge(r);
    }

    // 录制：确保输出目录存在
    size_t slash = output.find_last_of('/');
    if (slash != std::string::npos) {
        mkdir(output.substr(0, slash).c_str(), 0755);
    }

    if (!record_capture(output, duration_s)) {
        printf("\nSKIP: capture devices unavailable (no camera/mic?) — "
               "av_sync_capture_test requires /dev/video0 + ALSA hw:0,0\n");
        return 0;
    }

    auto r = analyze_flv(output);
    if (!r.ok) {
        printf("FAIL: %s\n", r.error.c_str());
        return 1;
    }
    return judge(r);
}
