# Linux 端详细架构设计

> 依赖文档：`docs/公共接口.md`（所有接口、类型、时钟、同步定义在此）
> 整体架构：`docs/总体架构.md`

---

## 1. 环境约束

| 项 | 现状 | 影响 |
|---|---|---|
| OS | Ubuntu 24.04 x86_64, kernel 6.17 | — |
| 编译器 | gcc/g++ 13.3, C++17 | `std::jthread`, `std::atomic_ref`, `std::optional` 可用 |
| 构建 | CMake 3.28 + GNU Make 4.3 + pkg-config 1.8 | — |
| FFmpeg | N-123159 自编译 (`--enable-debug`, **无** libx264) | AAC 编码可用，H.264 软件编码需重建 FFmpeg |
| libx264 | libx264-164 运行时已装，dev 头缺失 | `apt install libx264-dev` + 重建 FFmpeg |
| V4L2 | v4l2-ctl 1.26.1, 无 `/dev/video*` | 视频采集用文件/lavfi 模拟 |
| ALSA | pcmC0D0c (capture), pcmC0D0p/D1p (playback) | 音频采集和播放均可用 |
| 网络 | 需 Clash 代理 (127.0.0.1:7897) 访问外网 | SRS/Docker 拉取需代理，RTMP 本地推流不受影响 |
| SRS | 未安装 | Milestone 1 前置 |

### 关于 FFmpeg 重建

```bash
# 当前 FFmpeg 的 configure:
# --enable-debug
# 缺少: --enable-libx264 --enable-gpl

# 需要：
sudo apt install libx264-dev
cd <ffmpeg-source>
./configure --enable-debug --enable-libx264 --enable-gpl
make -j$(nproc)
sudo make install
```

验证：`ffmpeg -encoders 2>/dev/null | grep libx264` 应有输出。

---

## 2. Linux 目录结构

```text
linux/
├── CMakeLists.txt                    # 顶层，add_subdirectory 到 app/platform/tests
├── app/
│   ├── CMakeLists.txt
│   ├── main.cpp                      # 入口：解析 CLI、信号处理、组装 Session
│   ├── app_config.h                  # 从 CLI/配置文件构建 PublishSessionConfig
│   └── app_config.cpp
├── platform/                         # 实现 common/ 接口的 Linux 适配器
│   ├── CMakeLists.txt
│   ├── capture/
│   │   ├── ffmpeg_video_capture.h    # IVideoCapture: 文件/lavfi/V4L2 统一实现
│   │   ├── ffmpeg_video_capture.cpp
│   │   ├── ffmpeg_audio_capture.h    # IAudioCapture: 文件/lavfi/ALSA 统一实现
│   │   └── ffmpeg_audio_capture.cpp
│   ├── encode/
│   │   ├── ffmpeg_video_encoder.h    # IVideoEncoder: libx264 封装
│   │   ├── ffmpeg_video_encoder.cpp
│   │   ├── ffmpeg_audio_encoder.h    # IAudioEncoder: AAC 封装
│   │   └── ffmpeg_audio_encoder.cpp
│   ├── publish/
│   │   ├── ffmpeg_rtmp_publisher.h   # IMediaPublisher: FLV+RTMP 封装
│   │   └── ffmpeg_rtmp_publisher.cpp
│   ├── playback/                     # ★ 未来 Linux 播放端适配器
│   │   ├── ffmpeg_rtmp_subscriber.h  # IMediaSubscriber
│   │   ├── ffmpeg_video_decoder.h    # IVideoDecoder
│   │   ├── ffmpeg_audio_decoder.h    # IAudioDecoder
│   │   ├── sdl_video_renderer.h      # IVideoRenderer (SDL2)
│   │   ├── alsa_audio_output.h       # IAudioOutput (ALSA)
│   │   └── sdl_audio_output.h        # IAudioOutput (SDL2, 备选)
│   ├── clock/                        # ★ 时钟实现（Linux 和 Android 都可用，但放在 linux/ 是因为 Linux 为开发主平台）
│   │   ├── system_clock.h            # SystemMasterClock
│   │   └── system_clock.cpp
│   ├── device_probe.h                # V4L2/ALSA 设备能力探测
│   ├── device_probe.cpp
│   ├── ffmpeg_utils.h                # FFmpeg 对象 RAII 封装、AVRational 转换、错误转换
│   ├── ffmpeg_utils.cpp
│   ├── logger.h                      # ILogger 的 Linux 实现（stderr + 文件）
│   └── logger.cpp
└── tests/
    ├── CMakeLists.txt
    ├── test_common/                  # 测试 common/ 中的代码（队列、时间转换、状态机等）
    │   ├── test_media_queue.cpp
    │   ├── test_timestamp.cpp
    │   ├── test_result.cpp
    │   ├── test_stop_token.cpp
    │   └── test_av_sync.cpp
    ├── test_platform/                # 测试 Linux platform 适配器
    │   ├── test_device_probe.cpp
    │   ├── test_encoder.cpp
    │   ├── test_rtmp_publisher.cpp   # 需 SRS 环境
    │   └── test_capture.cpp
    └── fixtures/                     # 测试用媒体文件（小样本）
        ├── test_video_720p30.h264
        └── test_audio_48k.aac
```

**为什么 clock/ 放在 linux/ 而不是 common/**：`AudioMasterClock` 依赖 `IAudioOutput`（common 接口），但具体的 `SystemMasterClock` 不依赖任何平台 API。把它放在 `linux/` 是因为第一阶段 Linux 是唯一切实使用时钟的一端（发布端不需要时钟）。当 Android 端也需要时钟时，SystemMasterClock 的实现可以上移到 `common/src/`。

---

## 3. 推流链路（Phase 1 实现）

### 3.1 完整数据流

```text
┌──────────────────────────────────────────────────────────────────┐
│                        PublishSession                             │
│                                                                   │
│  ┌─────────────────┐          ┌─────────────────┐                │
│  │ FFmpegVideoCapture│         │ FFmpegAudioCapture│              │
│  │ source=文件/lavfi│         │ source=文件/lavfi│              │
│  │ 或 V4L2 avdevice │         │ 或 ALSA avdevice │              │
│  └────────┬────────┘          └────────┬────────┘                │
│           │ VideoFrame                 │ AudioFrame               │
│           │ pts=采集PTS                │ pts=采集PTS              │
│      ┌────▼───────────────┐     ┌──────▼─────────────┐          │
│      │ RawVideoQueue       │     │ RawAudioQueue      │          │
│      │ max=3 frames        │     │ max=200ms          │          │
│      │ 满→丢最旧帧         │     │ 满→短暂阻塞        │          │
│      └────┬───────────────┘     └──────┬─────────────┘          │
│           │                            │                          │
│      ┌────▼───────────────┐     ┌──────▼─────────────┐          │
│      │ FFmpegVideoEncoder  │     │ FFmpegAudioEncoder │          │
│      │ libx264 H.264       │     │ AAC-LC             │          │
│      │ 送帧→avcodec_send   │     │ 送帧→avcodec_send  │          │
│      │ 取包→avcodec_recv   │     │ 取包→avcodec_recv  │          │
│      │ 内部格式转换(sws)   │     │ 内部缓冲对齐       │          │
│      └────┬───────────────┘     └──────┬─────────────┘          │
│           │ MediaPacket                │ MediaPacket              │
│           │ H.264 ES, pts/dts         │ AAC ES, pts              │
│      ┌────▼──────────────────────────────▼─────────┐            │
│      │ VideoPacketQueue    AudioPacketQueue         │            │
│      │ max=2s              max=2s                   │            │
│      │ 满→丢非关键P帧                               │            │
│      └────┬──────────────────────────────┬─────────┘            │
│           │                              │                        │
│           └──────────┬───────────────────┘                        │
│                      ▼                                            │
│           ┌──────────────────────┐                                │
│           │ FFmpegRTMPPublisher   │                               │
│           │ avformat (flv+rtmp)  │                               │
│           │ 按 DTS 交织→av_inter │                               │
│           │ RTMP ts→av_pkt_rescale│                              │
│           └──────────┬───────────┘                                │
│                      │                                            │
└──────────────────────┼────────────────────────────────────────────┘
                       │ RTMP
                       ▼
                 ┌──────────┐
                 │   SRS    │
                 └──────────┘
```

### 3.2 每个节点的数据变换

```text
节点                      输入                  输出                    时间戳变化
──────────────────────────────────────────────────────────────────────────────
Capture (文件源)          mp4/flv 文件          VideoFrame(YUV420P)       pts = 文件 PTS → 归一化 → capture_pts_us
Capture (lavfi源)         filter描述            VideoFrame(YUV420P)       pts = frame_index * 33333us (30fps)
Capture (V4L2源)          /dev/video0           VideoFrame(YUYV422等)    pts = V4L2 buffer timestamp → monotonic?

Format Convert (编码器内) VideoFrame(任意fmt)   VideoFrame(YUV420P)       pts 透传，不变
Video Encoder             VideoFrame(YUV420P)   MediaPacket(H.264 ES)    pts = 输入 pts, dts = 编码器输出
Audio Encoder             AudioFrame(FLTP)      MediaPacket(AAC ES)      pts = 输入 pts
FLV Mux + RTMP            MediaPacket           RTMP chunks              pts_us → rtmp_ms = (pts_us - start_us)/1000
```

### 3.3 模块实现要点

**FFmpegVideoCapture**：

```
构造时传入 VideoCaptureConfig:
  - source = 文件路径  → avformat_open_input() + 找最佳视频流 + 创建 decoder
  - source = "lavfi:..." → avfilter_graph_create_filter("buffer") + lavfi src + buffersink
  - source = /dev/video0 → avformat_open_input("video4linux2", ...)

采集线程主循环:
  while (!stop_token.stop_requested()) {
    ret = av_read_frame(fmt_ctx, packet);  // 或 av_buffersink_get_frame
    if (ret == AVERROR_EOF) {
      if (loop) { av_seek_frame(..., 0); continue; }
      else { signal_eof(); break; }
    }
    // 解码:
    avcodec_send_packet(dec_ctx, packet);
    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
      VideoFrame vf = convert_avframe_to_videoframe(frame);
      vf.pts = normalize_pts(frame->pts, stream_time_base);
      on_frame(std::move(vf));
    }
  }

帧率控制 (模拟实时):
  target_interval_us = 1'000'000 / target_fps;
  每出一帧后 sleep(target_interval_us - elapsed_encode_us);
  // 可禁用（--no-throttle）用于性能测试
```

**FFmpegAudioCapture**：逻辑对称，但音频同步到视频时钟：不做独立帧率控制，跟随视频采集速率（如果音视频来自同一文件）或独立节流（如果分开）。

**FFmpegVideoEncoder**：

```
open():
  codec = avcodec_find_encoder_by_name("libx264");
  ctx = avcodec_alloc_context3(codec);
  ctx->width = config.width; ctx->height = config.height;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->time_base = {1, 1'000'000};  // 微秒 time base
  ctx->framerate = {30, 1};
  ctx->bit_rate = config.bitrate_bps;
  ctx->gop_size = config.gop_size;
  ctx->max_b_frames = 0;  // 关键：禁用 B 帧
  av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
  av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
  avcodec_open2(ctx, codec, nullptr);

encode(frame):
  1. 如果 frame.format != YUV420P: sws_scale() 转换
  2. AVFrame* avf = 从 frame 构造
  3. avf->pts = frame.pts.us;
  4. avcodec_send_frame(ctx, avf);
  5. while (avcodec_receive_packet(ctx, pkt) == 0):
       MediaPacket mp;
       mp.type = Video; mp.codec = H264;
       mp.pts = {pkt->pts}; mp.dts = {pkt->dts};
       mp.is_key_frame = (pkt->flags & AV_PKT_FLAG_KEY);
       mp.data.assign(pkt->data, pkt->data + pkt->size);
       packets.push_back(std::move(mp));
  6. return packets;

drain():
  同 encode 但没有输入帧: avcodec_send_frame(ctx, nullptr)
```

**FFmpegAudioEncoder**：逻辑对称，`AVCodecID::AV_CODEC_ID_AAC`。

关键差异：音频编码器需要帧对齐。

```
open():
  ...
  ctx->frame_size = 1024;  // AAC 标准帧大小
  ...

encode(frame):
  // 如果 frame.num_samples 不是 frame_size 的整数倍：
  //   拼接残余 samples
  //   每次取 frame_size 个 samples 编码
  //   保留不足 frame_size 的 samples 到下次
  fifo.push(frame);
  while (fifo.available() >= frame_size):
    subframe = fifo.pop(frame_size);
    ...编码 subframe...
```

**FFmpegRTMPPublisher**：

```
open(config):
  avformat_alloc_output_context2(&ctx, nullptr, "flv", config.url);
  // 创建音视频 AVStream
  video_stream = avformat_new_stream(ctx, nullptr);
  audio_stream = avformat_new_stream(ctx, nullptr);
  // 设置 codecpar (在 write_header 时)
  // 打开 IO
  avio_open2(&ctx->pb, config.url, AVIO_FLAG_WRITE,
             &interrupt_callback, &stop_token);

write_header(audio_info, video_info):
  // 从 StreamInfo 填充 AVStream.codecpar
  video_stream->codecpar->extradata = video_info.codec_extradata;
  ...
  avformat_write_header(ctx, nullptr);
  // 此时 FLV header + SPS/PPS + AudioSpecificConfig 已发送

write_packet(packet):
  AVPacket pkt;
  pkt.data = const_cast<uint8_t*>(packet.data.data());
  pkt.size = packet.data.size();
  pkt.pts = packet.pts.us;  // FFmpeg 期望微秒（我们设置了 {1,1000000} time base）
  pkt.dts = packet.dts.us;
  pkt.stream_index = packet.is_video() ? video_stream->index
                                       : audio_stream->index;
  pkt.flags = packet.is_key_frame ? AV_PKT_FLAG_KEY : 0;
  av_interleaved_write_frame(ctx, &pkt);  // 自动按 DTS 交织

close():
  av_write_trailer(ctx);
  avio_closep(&ctx->pb);
  avformat_free_context(ctx);
```

---

## 4. 线程模型（推流端）

### 4.1 线程清单

```text
┌─────────────────────────────────────────────────────────┐
│ Thread 0: main (control)                                │
│   职责:                                                  │
│   - 解析 CLI → AppConfig → PublishSessionConfig         │
│   - 创建 PublishSession                                  │
│   - 注册 SIGINT/SIGTERM → session.stop()                │
│   - 主循环: sleep(1s) → metrics() → 打印到日志         │
│   - 等待 session 线程退出                                │
│   权限: 启动/停止所有子线程                              │
│   阻塞点: session.stop() 等待子线程 join                 │
├─────────────────────────────────────────────────────────┤
│ Thread 1: video_capture                                 │
│   职责:                                                  │
│   - 循环 av_read_frame / av_buffersink_get_frame        │
│   - 解码 raw frames                                      │
│   - 归一化 PTS                                           │
│   - raw_video_queue.push(frame, timeout=100ms)           │
│   退出条件: stop_token || EOF（非循环）                  │
│   错误处理: push 超时记录 overrun，解码失败记录并继续    │
├─────────────────────────────────────────────────────────┤
│ Thread 2: audio_capture                                 │
│   职责: 同上，产出 AudioFrame → raw_audio_queue          │
│   特殊: 如音视频同源（一个文件），可以和 video_capture 合并│
│         为一个 demux 线程，分别输出到两个队列             │
├─────────────────────────────────────────────────────────┤
│ Thread 3: video_encode                                  │
│   职责:                                                  │
│   - raw_video_queue.pop(frame, timeout=5s)               │
│   - 格式转换 (按需 sws_scale)                            │
│   - 编码 → video_packet_queue.push(packets...)           │
│   退出条件: stop_token + queue empty                     │
│   错误处理: 编码失败 → 记录日志，继续（或进入 Error）    │
├─────────────────────────────────────────────────────────┤
│ Thread 4: audio_encode                                  │
│   职责: 同上，产出 AAC packets                           │
├─────────────────────────────────────────────────────────┤
│ Thread 5: mux_publish                                   │
│   职责:                                                  │
│   - 从 video_pkt_queue + audio_pkt_queue pop             │
│   - 比较 DTS，选较小的送出                               │
│   - av_interleaved_write_frame()                         │
│   - 处理网络 write 失败                                  │
│   退出条件: stop_token + 两队列均空                       │
│   错误处理: NetworkWriteFailed → session error / reconnect│
└─────────────────────────────────────────────────────────┘
```

### 4.2 线程安全合约

```text
每个线程操作的独占资源:
  video_capture  → FFmpegVideoCapture 的所有内部状态
  audio_capture  → FFmpegAudioCapture
  video_encode   → FFmpegVideoEncoder
  audio_encode   → FFmpegAudioEncoder
  mux_publish    → FFmpegRTMPPublisher

跨线程通信（仅通过 MediaQueue）:
  video_capture  ──VideoFrame──→ raw_video_queue  ──→ video_encode
  audio_capture  ──AudioFrame──→ raw_audio_queue   ──→ audio_encode
  video_encode   ──MediaPacket──→ video_pkt_queue  ──→ mux_publish
  audio_encode   ──MediaPacket──→ audio_pkt_queue  ──→ mux_publish

共享只读（所有线程可访问）:
  stop_token (atomic<bool>)
  config (read-only after prepare)
```

---

## 5. 推流会话生命周期

### 5.1 状态迁移实现

```cpp
// PublishSession 内部实现的状态处理
void PublishSession::stop() {
    SessionState expected = state_.load();
    // 幂等
    if (expected == SessionState::Stopped ||
        expected == SessionState::Idle) return;

    transition_to(SessionState::Stopping);

    // 1. 设置停止信号（必须最先，中断所有阻塞）
    stop_source_.request_stop();

    // 2. 中断 FFmpeg 网络 I/O
    publisher_->interrupt();

    // 3. abort 所有队列 → 唤醒所有阻塞线程
    raw_video_queue_.abort();
    raw_audio_queue_.abort();
    video_pkt_queue_.abort();
    audio_pkt_queue_.abort();

    // 4. 停止采集（停止产生新帧）
    video_capture_->stop();
    audio_capture_->stop();

    // 5. join 线程（顺序很重要）
    join_thread(video_capture_thread_, 3000ms);
    join_thread(audio_capture_thread_, 3000ms);
    // 编码线程看到 abort 的队列后自然退出
    join_thread(video_encode_thread_, 3000ms);
    join_thread(audio_encode_thread_, 3000ms);

    // 6. drain 编码器残余（可选）
    auto remaining = video_encoder_->drain();
    // ... 可选发送或丢弃

    // 7. 关闭 publisher
    publisher_->close();

    // 8. join mux 线程
    join_thread(mux_thread_, 5000ms);  // 网络可能需要较长时间

    // 9. 释放编码器和采集器
    video_encoder_->close();
    audio_encoder_->close();
    video_capture_->close();
    audio_capture_->close();

    transition_to(SessionState::Stopped);
}
```

### 5.2 异常路径

| 异常 | 检测位置 | 处理 |
|---|---|---|
| 文件 EOF（非循环） | video_capture thread | 推一个 sentinel 或设 eof_flag，mux 线程看到后正常 stop |
| 编码失败 | encode thread | 记录错误，跳过此帧继续。如果连续失败 > 10 帧 → session error |
| 网络写失败 | mux thread | 记录错误，publisher_->interrupt()，session error |
| 网络断开（ECONNRESET） | mux thread (av_interleaved_write_frame 返回 error) | session error。phase 1 不自动重连。 |
| 队列满 | capture thread | raw_video: 丢旧帧。raw_audio: 短暂阻塞后记录 overrun。 |
| 队列空 | encode thread | pop 超时 → 继续循环（可能是采集慢了） |
| SIGTERM | main thread | 同 stop() |

---

## 6. 未来播放端架构（Phase 2，只设计不实现）

Linux 作为播放端的架构。此节确保当前推流端的设计不会阻碍未来添加播放端。

### 6.1 播放数据流

```text
SRS → RTMP Subscribe → FLV Demux → MediaPacket
                                         │
                    ┌────────────────────┼────────────────────┐
                    ▼                    ▼                     ▼
             VideoPacketQueue    AudioPacketQueue              │
                    │                    │                     │
                    ▼                    ▼                     │
             FFmpegVideoDecoder  FFmpegAudioDecoder            │
                    │                    │                     │
                    ▼                    ▼                     │
          DecodedVideoQueue    DecodedAudioQueue               │
                    │                    │                     │
                    ▼                    ▼                     │
             AVSyncController ←── AudioMasterClock             │
                    │                    │                     │
                    ▼                    ▼                     │
             SDLVideoRenderer    ALSA/SDL AudioOutput          │
```

### 6.2 Linux 播放端实现映射

| Common 接口 | Linux 实现 | 理由 |
|---|---|---|
| `IMediaSubscriber` | `FFmpegRTMPSubscriber` | 复用 FFmpeg avformat RTMP 读取 |
| `IVideoDecoder` | `FFmpegVideoDecoder` | FFmpeg H.264 软件解码 |
| `IAudioDecoder` | `FFmpegAudioDecoder` | FFmpeg AAC 软件解码 |
| `IAudioOutput` | `ALSAAudioOutput` / `SDLAudioOutput` | ALSA 延迟更低，SDL2 更易用 |
| `IVideoRenderer` | `SDLVideoRenderer` | SDL2 跨平台窗口+渲染，OpenGL/纹理 |
| `IMediaClock` | `AudioMasterClock` | 音频主时钟，已在 公共接口.md 设计 |
| `IAVSyncController` | `ThresholdAVSyncController` | 已在 公共接口.md 设计 |

### 6.3 播放端线程模型

```text
main (control)
├── network_demux (1 thread)
│   read_packet() → video_pkt_queue + audio_pkt_queue
├── video_decode (1 thread)
│   video_pkt_queue.pop → decode → decoded_video_queue
├── audio_decode (1 thread)
│   audio_pkt_queue.pop → decode → decoded_audio_queue
├── audio_output (callback-based, AAudio/ALSA 管理的线程)
│   回调 → decoded_audio_queue.pop → submit to device
└── video_render (1 thread)
    decoded_video_queue.pop → av_sync.decide() → wait/render/drop
```

### 6.4 为什么推流端设计不阻碍播放端

| 检查项 | 当前设计 | 评估 |
|---|---|---|
| `MediaPacket` 可以携带解码后的帧吗? | MediaPacket 是编码数据包，VideoFrame/AudioFrame 是原始帧。播放端解码后产出 VideoFrame/AudioFrame。 | ✅ 类型系统已覆盖 |
| `PublishSession` 和 `PlaybackSession` 是否耦合? | 两个独立类，共享 `SessionState` 状态枚举和 `ISessionObserver`。 | ✅ 无耦合 |
| 时钟系统是否已设计? | `IMediaClock` + `SystemMasterClock` + `AudioMasterClock` + `IAVSyncController` + `ThresholdAVSyncController` 已设计 | ✅ 完整 |
| 播放端队列是否已定义? | 同一种 `MediaQueue<T>`，推流和播放端都使用 | ✅ 复用 |
| Native surface 如何传给播放端? | `NativeSurface` 结构 + `PlaybackSession::on_surface_changed()` | ✅ 接口预留 |
| Linux 音频输出如何适配? | `ALSAAudioOutput` 实现 `IAudioOutput` | ✅ 已规划 |

---

## 7. 错误处理体系

### 7.1 错误分类和处理策略

```cpp
// 错误按严重程度分级:
enum class ErrorSeverity {
    Recoverable,  // 单帧/单次操作失败，可跳过继续
    Degraded,     // 降级运行（如视频采集失败，仅推音频）
    Session,      // 当前 session 不可恢复，需要 stop + reset
    Fatal,        // 进程需要退出
};

// 每个错误的处理映射:
// DeviceNotFound     → Fatal (配置错误，不应自动修复)
// DeviceBusy         → Session (提示用户释放设备)
// DeviceDisconnected  → Session (运行时拔出)
// CodecEncodeFailed   → Recoverable (单帧跳过) → 连续 N 次 → Session
// CodecDecodeFailed   → Recoverable (单帧跳过) → 连续 N 次 → Session
// NetworkWriteFailed  → Session (phase 1) / Reconnecting (phase 2)
// NetworkDisconnected → Reconnecting (播放端)
// QueueAborted        → 正常停止路径，不算错误
// OutOfMemory         → Fatal
```

### 7.2 错误传播路径

```text
子线程中的错误如何处理:

1. 非致命错误 (Recoverable):
   - 记录日志
   - 增加错误计数器
   - 继续运行

2. 致命错误 (Session / Fatal):
   - 记录日志
   - 通过 error_queue (MediaQueue<ErrorEvent>) 或 atomic<ErrorCode>
     通知 control thread
   - control thread 调用 session.stop()
   - 主线程收到 SessionState::Error，打印汇总后退出

推流端: 任何网络致命错误 → Error → stop() → 进程退出(exit code 1)
播放端(未来): 网络致命错误 → Reconnecting（可恢复），超过重试次数 → Error
```

---

## 8. 配置管理

### 8.1 配置层次

```text
默认值 (hardcoded sensible defaults)
  ↓ 覆盖
配置文件 (~/.streambridge/config.toml, 可选 phase 2)
  ↓ 覆盖
命令行参数 (优先级最高)
  ↓
PublishSessionConfig / PlaybackSessionConfig (immutable after prepare)
```

### 8.2 CLI 设计

```bash
streambridge_publisher --help

推流端:
  streambridge_publisher \
    --rtmp-url rtmp://127.0.0.1:1935/live/stream0 \
    [--video-source <path|lavfi:desc|/dev/video0>] \
    [--audio-source <path|lavfi:desc|hw:0,0>] \
    [--no-video] [--no-audio] \
    [--video-width 1280] [--video-height 720] [--video-fps 30] \
    [--video-bitrate 2000000] [--video-preset veryfast] \
    [--audio-sample-rate 48000] [--audio-channels 2] \
    [--audio-bitrate 128000] \
    [--loop] [--no-throttle] \
    [--log-level info] [--log-file path/to/log]

播放端(未来):
  streambridge_player \
    --rtmp-url rtmp://127.0.0.1:1935/live/stream0 \
    [--output video=<sdl|drm>,audio=<alsa|sdl>] \
    [--av-sync-early 40000] [--av-sync-late -40000] [--av-sync-drop -120000] \
    [--log-level info]
```

### 8.3 配置验证

```cpp
Result<PublishSessionConfig> AppConfig::validate_and_build() {
    PublishSessionConfig config;

    // 1. 验证必填项
    if (rtmp_url_.empty())
        return Err(Config, InvalidUrl, "RTMP URL is required");

    // 2. 验证数值范围
    if (video_width_ < 320 || video_width_ > 3840)
        return Err(Config, InvalidConfig, "Width out of range [320, 3840]");
    if (video_fps_ < 1 || video_fps_ > 60)
        return Err(Config, InvalidConfig, "FPS out of range [1, 60]");

    // 3. 设置推导值
    config.video_encode.gop_size = video_fps_ * 2; // 2-second GOP

    // 4. 构建 config...
    return config;
}
```

---

## 9. 构建系统

### 9.1 CMake 结构

```cmake
# linux/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(StreamBridgeLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# 依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavcodec libavformat libavutil libavdevice libswscale libswresample)
pkg_check_modules(X264 REQUIRED x264)  # 可选，如果 FFmpeg 自己带了则不需要

# 线程 (C++17 std::jthread 不需要 -pthread 但 FFmpeg 内部需要)
find_package(Threads REQUIRED)

# common 库
add_subdirectory(${PROJECT_SOURCE_DIR}/../common common_build)
# common 的 CMakeLists.txt 生成 sb_common 静态库

# Linux 平台库
add_subdirectory(platform)

# 可执行文件
add_subdirectory(app)

# 测试
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

```cmake
# linux/platform/CMakeLists.txt
add_library(sb_linux_platform STATIC
    # capture
    capture/ffmpeg_video_capture.cpp
    capture/ffmpeg_audio_capture.cpp
    # encode
    encode/ffmpeg_video_encoder.cpp
    encode/ffmpeg_audio_encoder.cpp
    # publish
    publish/ffmpeg_rtmp_publisher.cpp
    # clock
    clock/system_clock.cpp
    # utils
    device_probe.cpp
    ffmpeg_utils.cpp
    logger.cpp
)
target_link_libraries(sb_linux_platform PUBLIC
    sb_common
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)
target_include_directories(sb_linux_platform PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # 访问 common/ 头文件
)
```

```cmake
# linux/app/CMakeLists.txt
add_executable(streambridge_publisher
    main.cpp
    app_config.cpp
)
target_link_libraries(streambridge_publisher PRIVATE
    sb_linux_platform
    sb_common
)
```

### 9.2 构建命令

```bash
# 配置
mkdir -p linux/build && cd linux/build
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 构建
cmake --build . -j$(nproc)

# 运行
./app/streambridge_publisher --help

# 测试
ctest --output-on-failure
```

---

## 10. 测试策略

### 10.1 单元测试（不需要硬件/网络）

```
test_media_queue:
  - push/pop 基本功能
  - 容量限制（满则阻塞/丢弃）
  - abort 后 push/pop 立即返回
  - flush 清空但不可 abort
  - shutdown 后行为
  - 并发: 2 写 2 读, 100k 条数据, 无 data race
  - 水位查询

test_timestamp:
  - TimePointUs / TimeDeltaUs 运算正确
  - Rational 转换: us ↔ ms ↔ rational
  - av_rescale_q 等价验证（与 FFmpeg 对比）
  - 首帧归一化: first_pts 之后所有 pts 减 first_pts
  - RTMP ms 往返: us → ms → us 误差 < 500us

test_av_sync:
  - 视频提前 > 40ms → Wait
  - 视频在范围内 → Render
  - 视频轻微落后 → Render (记录 late)
  - 视频严重落后 → Drop
  - 边界值精确测试(40ms, -40ms, -120ms)

test_result:
  - Result<T>::ok / err
  - value_or 回退
  - 错误信息链

test_stop_token:
  - 初始状态 stop_requested=false
  - request_stop() 后 stop_requested=true
  - 多线程并发 request_stop 安全
```

### 10.2 平台测试（需要 FFmpeg，不需要网络/硬件）

```
test_encoder:
  - 创建 FFmpegVideoEncoder, open(config)
  - 送一帧纯色 YUV → 获取 H.264 packet
  - 验证 packet.is_key_frame = true (首帧)
  - 验证 extradata 非空 (SPS/PPS)
  - drain: 获取尾巴 packets

  - 创建 FFmpegAudioEncoder, open(config)
  - 送一帧 silence FLTP → 获取 AAC packet
  - 验证 AudioSpecificConfig

test_capture:
  - 用 lavfi testsrc 做视频源: 采集 30 帧
  - 验证每帧有 pts, 单调递增
  - 验证格式是 YUV420P
  - 用 lavfi sine 做音频源: 采集 1 秒
  - 验证 num_samples, pts 合理
```

### 10.3 集成测试（需要 SRS）

```
test_rtmp_publisher:
  前置: SRS 运行在 localhost:1935
  - 创建完整的视频-only publish pipeline (lavfi testsrc → x264 → rtmp)
  - 推流 30 秒
  - 同时用 ffprobe 检查流存在、codec=H.264
  - 停止后 publisher stats 正确
```

### 10.4 端到端测试

```
e2e_test:
  前置: SRS 运行
  - streambridge_publisher 推流(lavfi), 推 60 秒
  - ffplay 拉流验证播放
  - 检查 publisher 日志中的 pts 连续、无异常时间戳跳跃
  - 内存泄漏检查 (valgrind 或 ASAN)
```

---

## 11. 观测性

### 11.1 日志实现

```cpp
// LinuxLogger: 实现 ILogger
class LinuxLogger : public ILogger {
public:
    // 输出到 stderr + 可选文件
    LinuxLogger(Level min_level, std::optional<std::string> file_path);

    void log(Level level, const char* module, const char* event,
             std::initializer_list<std::pair<const char*, std::string>> fields) override;

private:
    // 格式:
    // [2026-08-05T15:53:01.123456] [I] [capture.video] [thread_1] pts_us=15000000 frame=450
    std::string format(Level level, const char* module, const char* event,
                       std::initializer_list<...> fields);
    FILE* file_;
};
```

### 11.2 关键日志事件（推流端全覆盖）

```
# 启动阶段
session.lifecycle  state_from=Idle state_to=Preparing
device.probe       video_devices=0 audio_devices=1
capture.info       source=lavfi testsrc=size=1280x720:rate=30
encode.info        codec=libx264 preset=veryfast bitrate=2000000
publish.connect    url=rtmp://127.0.0.1:1935/live/stream0

# 运行阶段
session.lifecycle  state_from=Prepared state_to=Running
capture.video      pts_us=0 frame=0 size=1280x720 fmt=yuv420p
capture.audio      pts_us=0 samples=1024 rate=48000 ch=2
encode.video.in    pts_us=0 frame=0
encode.video.out   pts_us=0 dts_us=0 keyframe=1 bytes=23456
encode.audio.out   pts_us=0 bytes=456
publish.write      stream=video pts_us=0 rtmp_ms=0 bytes=23456
publish.write      stream=audio pts_us=0 rtmp_ms=0 bytes=456
queue.watermark    raw_video=2/3 raw_audio=5/10 video_pkt=15/30 audio_pkt=10/30

# 停止阶段
session.lifecycle  state_from=Running state_to=Stopping reason=user_signal
encode.drain       video_packets=2 audio_packets=1
publish.close      total_bytes=1234567 total_packets=1800
session.lifecycle  state_from=Stopping state_to=Stopped

# 汇总
=== Summary ===
duration_s: 30.5
video_frames_captured: 915
video_frames_encoded: 915
audio_frames_captured: 1464
audio_frames_encoded: 1464
total_bytes_sent: 5242880
avg_encode_video_us: 2300
max_queue_depth_video: 2
drops: 0
exit_code: 0
```

### 11.3 指标

推流端定期（每 2 秒）打印：

```text
[METRICS] uptime=30s video_cap=900 video_enc=900 vid_bytes=4500KB
  audio_cap=1440 audio_enc=1440 aud_bytes=28KB
  q_raw_v=1/3 q_raw_a=3/10 q_vpkt=12/30 q_apkt=8/30
  enc_vid_avg=2.3ms enc_vid_max=8.1ms
  net_bw=1200kbps
```

---

## 12. main() 伪代码

```cpp
int main(int argc, char* argv[]) {
    // 1. 解析命令行
    auto config_result = AppConfig::parse(argc, argv);
    if (config_result.is_err()) {
        std::cerr << config_result.error_message() << "\n";
        AppConfig::print_usage();
        return 1;
    }
    auto app_cfg = config_result.value();

    auto session_cfg = app_cfg.to_session_config();
    if (session_cfg.is_err()) {
        std::cerr << "Invalid config: " << session_cfg.error_message() << "\n";
        return 1;
    }

    // 2. 初始化日志
    LinuxLogger logger(app_cfg.log_level, app_cfg.log_file);

    // 3. 创建平台适配器（依赖注入）
    auto video_cap = std::make_unique<FFmpegVideoCapture>();
    auto audio_cap = std::make_unique<FFmpegAudioCapture>();
    auto video_enc = std::make_unique<FFmpegVideoEncoder>();
    auto audio_enc = std::make_unique<FFmpegAudioEncoder>();
    auto publisher = std::make_unique<FFmpegRTMPPublisher>();

    PublishSession session(
        std::move(video_cap), std::move(audio_cap),
        std::move(video_enc), std::move(audio_enc),
        std::move(publisher));

    // 4. 设置观察者
    auto observer = std::make_shared<LoggingSessionObserver>(logger);
    session.set_observer(observer);

    // 5. Prepare
    auto result = session.prepare(session_cfg.value());
    if (result.is_err()) {
        LOG_E("main", "session.prepare_error", "error", result.to_string());
        return 1;
    }

    // 6. 信号处理
    std::atomic<bool> running{true};
    signal(SIGINT, [](int) { running = false; });
    signal(SIGTERM, [](int) { running = false; });

    // 7. 启动
    result = session.start();
    if (result.is_err()) {
        LOG_E("main", "session.start_error", "error", result.to_string());
        return 1;
    }

    // 8. 主循环
    while (running && session.state() == SessionState::Running) {
        sleep(1);
        auto m = session.metrics();
        // 打印 metrics...
    }

    // 9. 停止
    session.stop();
    LOG_I("main", "session_stopped");

    return 0;
}
```

---

## 13. Pimpl 惯用法

为保持公共头文件不泄漏 FFmpeg 类型，所有 Linux 平台实现类使用 Pimpl（Pointer to Implementation）：

```cpp
// ffmpeg_video_encoder.h (public header, included by app)
#include "streambridge/codec.h"  // IVideoEncoder

class FFmpegVideoEncoder : public IVideoEncoder {
public:
    FFmpegVideoEncoder();
    ~FFmpegVideoEncoder() override;

    Result<void> open(const VideoEncodeConfig& config) override;
    Result<std::vector<MediaPacket>> encode(VideoFrame frame) override;
    // ...
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // AVCodecContext* 等 FFmpeg 对象在 Impl 中
};

// ffmpeg_video_encoder.cpp (implementation, includes FFmpeg headers)
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

struct FFmpegVideoEncoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;   // 格式转换
    AVFrame* converted = nullptr;
    // ...
};
```

这样 `common/include/` 和 `linux/app/` 不需要 include FFmpeg 头文件。

---

## 14. FFmpeg 工具函数

```cpp
// ffmpeg_utils.h — 所有 FFmpeg 对象的 RAII 封装

// AVFormatContext: avformat_close_input
using AVFormatContextPtr = std::unique_ptr<AVFormatContext,
    decltype(&avformat_close_input)>;
inline auto make_format_context() {
    return AVFormatContextPtr(avformat_alloc_context(), &avformat_free_context);
}

// AVCodecContext: avcodec_free_context
using AVCodecContextPtr = std::unique_ptr<AVCodecContext,
    decltype(&avcodec_free_context)>;

// AVFrame: av_frame_free
using AVFramePtr = std::unique_ptr<AVFrame, decltype(&av_frame_free)>;
inline AVFramePtr make_avframe() {
    return AVFramePtr(av_frame_alloc(), &av_frame_free);
}

// AVPacket: av_packet_free
using AVPacketPtr = std::unique_ptr<AVPacket, decltype(&av_packet_free)>;
inline AVPacketPtr make_avpacket() {
    return AVPacketPtr(av_packet_alloc(), &av_packet_free);
}

// --- 转换函数 ---

// AVRational → Rational
inline Rational from_avrational(AVRational r) { return {r.num, r.den}; }

// timestamp 转换: pts_in_src_tb → us
inline int64_t to_us(int64_t pts, AVRational tb) {
    return av_rescale_q(pts, tb, {1, 1'000'000});
}

// us → dst_tb
inline int64_t from_us(int64_t us, AVRational tb) {
    return av_rescale_q(us, {1, 1'000'000}, tb);
}

// VideoFrame ↔ AVFrame
VideoFrame from_avframe(AVFrame* avf, AVRational tb);
AVFrame* to_avframe(const VideoFrame& vf);  // caller owns

// AudioFrame ↔ AVFrame
AudioFrame from_avframe_audio(AVFrame* avf, AVRational tb);
AVFrame* to_avframe_audio(const AudioFrame& af);

// AV error code → Result error
ErrorCode from_averror(int averr);
```

---

## 15. 单边验证（M1-M3）

### M1: FFmpeg 命令验证（不写 C++）

```bash
# 前提: SRS 已启动, FFmpeg 有 libx264
# 验证: 本地文件 → RTMP → ffplay

# 推流
ffmpeg -re -stream_loop -1 -i sample.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -x264-params "keyint=60:no-b-adapt=1:bframes=0" \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://127.0.0.1:1935/live/test

# 拉流验证（另一个终端）
ffplay rtmp://127.0.0.1:1935/live/test

# 流信息检查
ffprobe -v quiet -print_format json -show_streams \
  rtmp://127.0.0.1:1935/live/test

# 预期结果:
# - ffplay 能稳定播放
# - codec_name: h264, aac
# - 视频 width=1280, height=720 (或其他)
# - 音频 sample_rate=48000, channels=2
```

### M2: C++ 程序，lavfi 视频源

```bash
# 构建
cd linux/build && cmake .. && make -j$(nproc)

# 推流 (lavfi 测试画面)
./app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/test \
  --video-source "lavfi:Testsrc=size=1280x720:rate=30" \
  --no-audio \
  --log-level info

# 单边验证
ffplay rtmp://127.0.0.1:1935/live/test
# 预期: 看到测试画面(彩色条纹+时间戳)
```

### M3: C++ 程序，lavfi 视频 + 真实/模拟音频

```bash
# 推流
./app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/test \
  --video-source "lavfi:Testsrc=size=1280x720:rate=30" \
  --audio-source "lavfi:Sine=frequency=440:sample_rate=48000" \
  --log-level info

# 或使用 ALSA 麦克风（如果可用）
./app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/test \
  --video-source "lavfi:Testsrc=size=1280x720:rate=30" \
  --audio-source "hw:0,0" \
  --log-level info

# 验证
ffplay rtmp://127.0.0.1:1935/live/test
# 预期: 测试画面 + 440Hz 纯音 / 麦克风声音
```

---

## 16. Git 分支策略（Linux AI）

```bash
# 当前
git checkout -b linux/build-integration

# 完成的提交示例:
# 1. feat: add common CMakeLists.txt and MediaQueue
# 2. feat: add FFmpeg utils (RAII wrappers, conversions)
# 3. feat: add FFmpegVideoEncoder and FFmpegAudioEncoder
# 4. feat: add FFmpegRTMPPublisher
# 5. feat: add FFmpegVideoCapture and FFmpegAudioCapture
# 6. feat: add PublishSession with thread management
# 7. feat: add CLI and main.cpp
# 8. test: add unit tests for MediaQueue and timestamp
# 9. feat: add logging infrastructure
```

推送前先确保 `shared/interface-contract` 分支上的 common 头文件已稳定。

---

## 17. 风险与缓解

| 风险 | 严重度 | 缓解 |
|---|---|---|
| FFmpeg 阻塞调用无法中断 | 高 | 用 `AVIOInterruptCB` + StopToken，代码中测试中断响应时间 < 2s |
| 文件/lavfi 源与实际设备行为不一致 | 中 | 用相同接口，M2/M3 阶段增加真实设备验证。设计上接口不区分源类型。 |
| VM 无摄像头影响 M2 验收 | 中 | ✅ 已确认用文件/lavfi 代替 |
| libx264 编码延迟波动 | 中 | 用 `zerolatency` tune，记录每帧编码耗时，超阈值告警 |
| AAC 帧对齐复杂 | 低 | 实现内部 FIFO 缓冲，unit test 覆盖各种 num_samples |
| 虚拟机时钟不单调 | 低 | 使用 `CLOCK_MONOTONIC`（不受墙钟调整影响） |

---

## 附录 A: 当前阻塞项

| 项 | 状态 | 阻塞什么 |
|---|---|---|
| 架构确认 | ⚠️ 等待用户 | 所有后续步骤 |
| SRS 安装 | ⚠️ 待决定 | M1 |
| libx264 重建 FFmpeg | ⚠️ 待用户确认 | M2+ |

## 附录 B: 已确认项

| 项 | 决定 |
|---|---|
| 无摄像头时的视频源 | 使用本地文件和 lavfi 测试源 ✅ |
| VM 环境工作 | 可以，单边验证用 ffplay ✅ |
| C++ 标准 | C++17 ✅ |
| 构建系统 | CMake ✅ |
| 禁用 B 帧 | 是 ✅ |
