// StreamBridge Linux Player — 反向链路：拉流（RTMP）→ 解码 → 同步 → SDL 渲染 + ALSA 播放
// 线程模型：
//   main         — 事件轮询 / 指标打印 / 生命周期
//   demux        — FFmpegSubscriber 读包 → 音视频压缩包队列（满时等待，不丢包）
//   audio        — 解码 → ALSA 阻塞写 → 更新 MediaClock（音频主时钟）
//   video        — 解码 → AVSyncController 决策（Wait/Render/RenderLate/Drop）→ SDL 渲染
//
// 单边验证（不依赖 Android 推流端）：
//   ffmpeg -re -i sample.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/test
//   ./player --url rtmp://127.0.0.1:1935/live/test --duration 15

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include "streambridge/av_sync.h"
#include "streambridge/logging.h"
#include "streambridge/media_queue.h"
#include "streambridge/stop_token.h"
#include "../platform/output/alsa_audio_output.h"
#include "../platform/render/sdl_video_renderer.h"
#include "ffmpeg/ffmpeg_audio_decoder.h"
#include "ffmpeg/ffmpeg_subscriber.h"
#include "ffmpeg/ffmpeg_video_decoder.h"

using namespace streambridge;

namespace {

// ============================================================
// 播放器上下文
// ============================================================
struct Player {
    std::string url;

    ffmpeg::FFmpegSubscriber subscriber;
    ffmpeg::FFmpegVideoDecoder video_decoder;
    ffmpeg::FFmpegAudioDecoder audio_decoder;
    SDLVideoRenderer renderer;
    ALSAAudioOutput audio_output;

    // 压缩包队列：ByDuration 2s；满时等待、绝不静默丢包（H.264 参考链，见 troubleshooting 问题 10）
    MediaQueue<MediaPacket> video_pkt_q{MediaQueue<MediaPacket>::Config{
        0, TimeDeltaUs{2'000'000}, MediaQueue<MediaPacket>::CapacityMode::ByDuration,
        false, TimeDeltaUs{100'000}, TimeDeltaUs{5'000'000}}};
    MediaQueue<MediaPacket> audio_pkt_q{MediaQueue<MediaPacket>::Config{
        0, TimeDeltaUs{2'000'000}, MediaQueue<MediaPacket>::CapacityMode::ByDuration,
        false, TimeDeltaUs{100'000}, TimeDeltaUs{5'000'000}}};

    MediaClock clock;
    AVSyncController sync;

    StopSource stop_source;

    std::thread demux_thread;
    std::thread video_thread;
    std::thread audio_thread;

    bool has_audio = false;

    // 统计
    std::atomic<int64_t> frames_rendered{0};
    std::atomic<int64_t> frames_dropped{0};
    std::atomic<int64_t> audio_frames_played{0};
    std::atomic<int64_t> last_av_diff_us{0};
    std::atomic<int> reconnects{0};
};

// ============================================================
// demux 线程：读包入队，断流重连
// ============================================================
void demux_loop(Player& p) {
    SB_LOG_I("demux", "demux thread started");
    while (!p.stop_source.stop_requested()) {
        auto pkt_res = p.subscriber.read_packet();
        if (pkt_res.is_err()) {
            SB_LOG_E("demux", "read_packet error: %s", pkt_res.error_message().c_str());
            if (p.stop_source.stop_requested()) break;
            // 断流重连：清队列、关解码器、重新打开订阅
            SB_LOG_W("demux", "reconnecting in 2s...");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (p.stop_source.stop_requested()) break;
            p.subscriber.close();
            auto ret = p.subscriber.open(p.url);
            if (ret.is_err()) {
                SB_LOG_E("demux", "reconnect failed: %s", ret.error_message().c_str());
                continue;
            }
            p.video_pkt_q.flush();
            p.audio_pkt_q.flush();
            p.video_decoder.flush();
            p.audio_decoder.flush();
            p.clock.reset();
            p.reconnects++;
            continue;
        }

        MediaPacket pkt = *pkt_res;
        if (pkt.is_video()) {
            // 满时阻塞等待（队列有 2s 时长上限，消费者正常时不会满）
            auto res = p.video_pkt_q.push(std::move(pkt), TimeDeltaUs::from_ms(500));
            if (res == QueueResult::Aborted) break;
            if (res == QueueResult::Timeout) {
                SB_LOG_W("demux", "video pkt queue full for 500ms — dropping (queue stalled)");
            }
        } else if (p.has_audio) {
            auto res = p.audio_pkt_q.push(std::move(pkt), TimeDeltaUs::from_ms(500));
            if (res == QueueResult::Aborted) break;
            if (res == QueueResult::Timeout) {
                SB_LOG_W("demux", "audio pkt queue full for 500ms — dropping (queue stalled)");
            }
        }
    }
    // 通知队列结束
    p.video_pkt_q.abort();
    p.audio_pkt_q.abort();
    SB_LOG_I("demux", "demux thread exiting");
}

// ============================================================
// audio 线程：解码 → ALSA 播放 → 更新音频主时钟
// ============================================================
void audio_loop(Player& p) {
    SB_LOG_I("audio", "audio decode/output thread started");
    bool first_frame = true;
    TimePointUs first_audio_pts{0};

    while (!p.stop_source.stop_requested()) {
        MediaPacket pkt;
        auto res = p.audio_pkt_q.pop(pkt, TimeDeltaUs::from_ms(5000));
        if (res == QueueResult::Aborted) break;
        if (res == QueueResult::Timeout) continue;

        auto send_ret = p.audio_decoder.send_packet(pkt);
        if (send_ret.is_err()) continue;

        while (true) {
            auto dec = p.audio_decoder.receive_frame();
            if (dec.is_err() || !dec->has_frame) break;

            AudioFrame frame = std::move(dec->frame);
            if (first_frame) {
                first_audio_pts = frame.pts;
                p.clock.start(first_audio_pts);
                first_frame = false;
            }
            auto w = p.audio_output.write(frame);
            if (w.is_err()) {
                SB_LOG_E("audio", "ALSA write error: %s", w.error_message().c_str());
                break;
            }
            p.audio_frames_played++;
            // 用设备实际播放进度更新主时钟
            p.clock.update_audio(first_audio_pts,
                                 p.audio_output.played_frames(),
                                 p.audio_output.sample_rate());
        }
    }
    SB_LOG_I("audio", "audio thread exiting");
}

// ============================================================
// video 线程：解码 → 同步决策 → 渲染
// ============================================================
void video_loop(Player& p) {
    SB_LOG_I("video", "video decode/render thread started");
    bool clock_started = false;

    while (!p.stop_source.stop_requested()) {
        MediaPacket pkt;
        auto res = p.video_pkt_q.pop(pkt, TimeDeltaUs::from_ms(5000));
        if (res == QueueResult::Aborted) break;
        if (res == QueueResult::Timeout) continue;

        auto send_ret = p.video_decoder.send_packet(pkt);
        if (send_ret.is_err()) continue;

        while (true) {
            auto dec = p.video_decoder.receive_frame(5);
            if (dec.is_err()) break;
            if (!dec->payload.valueless_by_exception()) {
                auto* cpu = std::get_if<CpuFrameHandle>(&dec->payload);
                if (!cpu) continue;  // 第一版只支持 CPU 帧
                VideoFrame frame = cpu->frame;
                TimePointUs pts{dec->pts_us};

                // 无音频流（video-only）时主时钟必须以首帧启动：
                // MediaClock 未启动时 now() 恒为 0，所有帧都会被判为
                // 「超前」而长时间 Wait。有音频时音频线程会接管时钟。
                if (!clock_started) {
                    p.clock.start(pts);
                    clock_started = true;
                    SB_LOG_I("video", "master clock started from first video pts=%lld",
                          static_cast<long long>(pts.us));
                }

                // 同步决策：视频 PTS vs 音频主时钟
                auto d = p.sync.decide(pts, p.clock.now());
                p.last_av_diff_us.store(d.av_diff_us);

                switch (d.action) {
                    case VideoSyncAction::Wait:
                        std::this_thread::sleep_for(std::chrono::microseconds(d.wait_us));
                        [[fallthrough]];
                    case VideoSyncAction::Render:
                    case VideoSyncAction::RenderLate: {
                        auto r = p.renderer.render(frame);
                        if (r.is_err()) {
                            SB_LOG_E("video", "render error: %s", r.error_message().c_str());
                        } else {
                            p.frames_rendered++;
                        }
                        break;
                    }
                    case VideoSyncAction::Drop:
                        p.frames_dropped++;
                        break;
                }
            }
        }
    }
    SB_LOG_I("video", "video thread exiting");
}

// ============================================================
// 命令行
// ============================================================
void usage(const char* prog) {
    fprintf(stderr,
        "StreamBridge Linux Player — RTMP 拉流播放（反向链路验证）\n"
        "Usage: %s --url <rtmp://...> [options]\n"
        "  --url <url>          RTMP 流地址（必填）\n"
        "  --audio-device <dev> ALSA 播放设备（默认 default）\n"
        "  --no-audio           禁用音频播放\n"
        "  --window <WxH>       窗口尺寸（默认 1280x720）\n"
        "  --duration <s>       自动退出秒数（0=直到 ESC/关闭窗口，默认 0）\n"
        "  --log-level <level>  debug/info/warn/error（默认 info）\n", prog);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string url;
    std::string audio_device = "default";
    bool no_audio = false;
    bool test_pattern = false;
    int win_w = 1280, win_h = 720;
    int duration_s = 0;
    LogLevel log_level = LogLevel::Info;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (arg == "--url" && (argv[i+1] != nullptr)) url = argv[++i];
        else if (arg == "--audio-device") { const char* v = next(); if (v) audio_device = v; }
        else if (arg == "--no-audio") no_audio = true;
        else if (arg == "--test-pattern") test_pattern = true;
        else if (arg == "--window") {
            const char* v = next();
            if (v) sscanf(v, "%dx%d", &win_w, &win_h);
        }
        else if (arg == "--duration") { const char* v = next(); if (v) duration_s = atoi(v); }
        else if (arg == "--log-level") {
            const char* v = next();
            if (v) {
                if (!strcmp(v, "debug")) log_level = LogLevel::Debug;
                else if (!strcmp(v, "warn")) log_level = LogLevel::Warn;
                else if (!strcmp(v, "error")) log_level = LogLevel::Error;
            }
        }
        else { usage(argv[0]); return 2; }
    }
    if (url.empty() && !test_pattern) { usage(argv[0]); return 2; }
    set_log_level(log_level);

    Player p;
    p.url = url;

    // --test-pattern：不拉流，渲染纯色测试帧（红→绿→蓝→白），
    // 用于验证渲染/呈现链路与颜色通道映射（黑屏排查）
    if (test_pattern) {
        auto ret = p.renderer.open("StreamBridge Player", win_w, win_h);
        if (ret.is_err()) {
            SB_LOG_E("main", "renderer open failed: %s", ret.error_message().c_str());
            return 1;
        }
        const uint32_t colors[4] = {0x000000FF, 0x0000FF00, 0x00FF0000, 0x00FFFFFF};  // RGBA 内存序
        for (int c = 0; c < 4; c++) {
            VideoFrame vf;
            auto buf = std::make_shared<CpuFrameBuffer>(win_w * win_h * 4);
            uint32_t* px = reinterpret_cast<uint32_t*>(buf->data());
            for (int i = 0; i < win_w * win_h; i++) px[i] = colors[c];
            vf.format = PixelFormat::RGBA;
            vf.width = win_w;
            vf.height = win_h;
            vf.num_planes = 1;
            vf.planes[0] = {buf->data(), win_w * static_cast<size_t>(win_h) * 4, win_w * 4, 0};
            vf.buffer = buf;
            SB_LOG_I("main", "test pattern #%d (0x%08x)", c, colors[c]);
            p.renderer.render(vf);
            for (int s = 0; s < 20 && !p.stop_source.stop_requested(); s++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        p.renderer.close();
        SB_LOG_I("main", "test pattern done");
        return 0;
    }

    // 0. 先打开渲染窗口（窗口立即可见，避免拉流慢时黑屏等待）
    auto render_ret = p.renderer.open("StreamBridge Player", win_w, win_h);
    if (render_ret.is_err()) {
        SB_LOG_E("main", "renderer open failed: %s", render_ret.error_message().c_str());
        return 1;
    }

    // 1. 打开拉流
    auto ret = p.subscriber.open(url);
    if (ret.is_err()) {
        SB_LOG_E("main", "cannot open stream: %s", ret.error_message().c_str());
        return 1;
    }
    SB_LOG_I("main", "stream opened: video=%s audio=%s",
          p.subscriber.has_video() ? "yes" : "no",
          p.subscriber.has_audio() ? "yes" : "no");

    // 2. 打开解码器
    if (p.subscriber.video_stream()) {
        ret = p.video_decoder.open(*p.subscriber.video_stream());
        if (ret.is_err()) {
            SB_LOG_E("main", "video decoder open failed: %s", ret.error_message().c_str());
            return 1;
        }
    }
    p.has_audio = !no_audio && p.subscriber.has_audio();
    if (p.has_audio) {
        ret = p.audio_decoder.open(*p.subscriber.audio_stream());
        if (ret.is_err()) {
            SB_LOG_E("main", "audio decoder open failed: %s", ret.error_message().c_str());
            return 1;
        }
    }

    // 3. 打开音频输出（渲染窗口已在最前面打开）
    if (p.has_audio) {
        const StreamInfo* asi = p.subscriber.audio_stream();
        ret = p.audio_output.open(audio_device, asi->sample_rate, asi->channels);
        if (ret.is_err()) {
            SB_LOG_E("main", "audio output open failed: %s", ret.error_message().c_str());
            return 1;
        }
    }

    // 4. 启动工作线程
    p.demux_thread = std::thread(demux_loop, std::ref(p));
    if (p.has_audio) p.audio_thread = std::thread(audio_loop, std::ref(p));
    p.video_thread = std::thread(video_loop, std::ref(p));

    // 5. 主循环：事件 + 指标
    SB_LOG_I("main", "playing — press ESC or q to quit");
    auto start_time = std::chrono::steady_clock::now();
    auto deadline = (duration_s > 0)
        ? start_time + std::chrono::seconds(duration_s)
        : std::chrono::steady_clock::time_point::max();

    bool quit = false;
    long last_printed = -1;
    while (!quit && !p.stop_source.stop_requested()) {
        bool user_quit = false;
        p.renderer.poll_events(user_quit);
        if (user_quit) break;

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            SB_LOG_I("main", "duration reached, exiting");
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed % 2 == 0 && elapsed != last_printed) {
            last_printed = elapsed;
            SB_LOG_I("main", "uptime=%lds rendered=%ld dropped=%ld av_diff_us=%lld "
                  "audio_frames=%ld vq=%zu aq=%zu reconnects=%d",
                  elapsed, p.frames_rendered.load(), p.frames_dropped.load(),
                  static_cast<long long>(p.last_av_diff_us.load()),
                  p.audio_frames_played.load(),
                  p.video_pkt_q.size(), p.audio_pkt_q.size(),
                  p.reconnects.load());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 6. 停止：先置停止标志并中断订阅器 IO，再等待线程退出后清理
    SB_LOG_I("main", "stopping...");
    p.stop_source.request_stop();
    p.subscriber.interrupt();  // 唤醒阻塞中的 read_packet
    if (p.demux_thread.joinable()) p.demux_thread.join();
    if (p.audio_thread.joinable()) p.audio_thread.join();
    if (p.video_thread.joinable()) p.video_thread.join();

    p.audio_output.close();
    p.renderer.close();
    SB_LOG_I("main", "stopped — rendered=%ld dropped=%ld audio_frames=%ld reconnects=%d",
          p.frames_rendered.load(), p.frames_dropped.load(),
          p.audio_frames_played.load(), p.reconnects.load());
    return 0;
}
