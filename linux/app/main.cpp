// StreamBridge Linux Publisher — M3: video + optional audio RTMP push
#include <atomic>
#include <csignal>
#include <thread>

#include "../platform/logger.h"
#include "../platform/capture/ffmpeg_video_capture.h"
#include "../platform/capture/v4l2_video_capture.h"
#include "../platform/capture/ffmpeg_audio_capture.h"
#ifdef STREAMBRIDGE_HAS_ALSA
#include "../platform/capture/alsa_audio_capture.h"
#endif
#include "../platform/encode/ffmpeg_video_encoder.h"
#include "../platform/encode/ffmpeg_audio_encoder.h"
#include "../platform/publish/ffmpeg_rtmp_publisher.h"

#include "app_config.h"
#include "streambridge/session.h"

using namespace streambridge;

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    // 1. 解析命令行
    auto cfg_result = AppConfig::parse(argc, argv);
    if (cfg_result.is_err()) {
        fprintf(stderr, "Error: %s\n", cfg_result.error_message().c_str());
        print_usage(argv[0]);
        return 1;
    }
    auto app_cfg = *cfg_result;

    // 2. 设置日志级别
    if (app_cfg.log_level_str == "debug") set_log_level(LogLevel::Debug);
    else if (app_cfg.log_level_str == "warn") set_log_level(LogLevel::Warn);
    else if (app_cfg.log_level_str == "error") set_log_level(LogLevel::Error);
    else set_log_level(LogLevel::Info);

    SB_LOG_I("main", "StreamBridge Publisher starting (M3)");
    SB_LOG_I("main", "rtmp-url=%s video=%s %dx%d@%dfps bitrate=%d audio=%s",
          app_cfg.rtmp_url.c_str(), app_cfg.video_source.c_str(),
          app_cfg.video_width, app_cfg.video_height, app_cfg.video_fps,
          app_cfg.video_bitrate,
          app_cfg.enable_audio ? app_cfg.audio_source.c_str() : "disabled");

    // 3. 创建平台适配器（依赖注入）
    std::unique_ptr<IVideoCapture> video_cap;
    if (app_cfg.video_backend == "v4l2") {
        video_cap = std::make_unique<V4L2VideoCapture>();
        SB_LOG_I("main", "using V4L2 video backend");
    } else {
        SB_LOG_I("main", "using FFmpeg video backend");
        video_cap = std::make_unique<FFmpegVideoCapture>();
    }
    auto video_enc = std::make_unique<FFmpegVideoEncoder>();
    auto publisher = std::make_unique<FFmpegRTMPPublisher>();

    std::unique_ptr<IAudioCapture> audio_cap;
    std::unique_ptr<IAudioEncoder> audio_enc;
    if (app_cfg.enable_audio) {
        if (app_cfg.audio_backend == "alsa") {
#ifdef STREAMBRIDGE_HAS_ALSA
            audio_cap = std::make_unique<ALSAAudioCapture>();
            SB_LOG_I("main", "using ALSA audio backend");
#else
            SB_LOG_W("main", "ALSA not compiled in, falling back to FFmpeg/lavfi");
            audio_cap = std::make_unique<FFmpegAudioCapture>();
#endif
        } else {
            // "lavfi" or "file" — both handled by FFmpegAudioCapture
            audio_cap = std::make_unique<FFmpegAudioCapture>();
        }
        audio_enc = std::make_unique<FFmpegAudioEncoder>();
    }

    PublishSession session(
        std::move(video_cap), std::move(audio_cap),
        std::move(video_enc), std::move(audio_enc),
        std::move(publisher));

    // 4. 准备
    auto session_cfg = app_cfg.to_session_config();
    auto ret = session.prepare(session_cfg);
    if (ret.is_err()) {
        SB_LOG_E("main", "prepare failed: %s", ret.to_string().c_str());
        return 1;
    }
    SB_LOG_I("main", "prepared OK (audio=%s)",
          app_cfg.enable_audio ? "enabled" : "disabled");

    // 5. 信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 6. 启动
    ret = session.start();
    if (ret.is_err()) {
        SB_LOG_E("main", "start failed: %s", ret.to_string().c_str());
        return 1;
    }
    SB_LOG_I("main", "running — press Ctrl+C to stop");

    // 7. 主循环：定期打印指标
    auto start_time = std::chrono::steady_clock::now();
    while (g_running && session.state() == SessionState::Running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto m = session.metrics();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        SB_LOG_I("main", "uptime=%lds cap=%ld enc=%ld sent=%ld "
              "drop=%ld q_raw_v=%zu q_raw_a=%zu q_pkt_v=%zu q_pkt_a=%zu bytes=%ld",
              elapsed, m.frames_captured, m.frames_encoded,
              m.packets_sent, m.frames_dropped,
              m.raw_video_queue, m.raw_audio_queue,
              m.video_pkt_queue, m.audio_pkt_queue,
              m.bytes_sent);

        if (session.state() == SessionState::Error) {
            SB_LOG_E("main", "session error, exiting");
            break;
        }
    }

    // 8. 停止
    SB_LOG_I("main", "stopping...");
    session.stop();
    SB_LOG_I("main", "exiting");
    return 0;
}
