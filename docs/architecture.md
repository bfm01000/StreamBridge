# StreamBridge Architecture

本文档定义 StreamBridge 第一阶段的整体架构。当前仓库尚无 `common/`、`linux/`、`android/` 源码目录和构建文件，因此本文中的目录和接口均为架构规划，不代表已经实现。

> 配套文档：
> - `docs/common-interfaces.md` — 公共层完整接口体系（类型、采集、编解码、传输、输出、时钟、同步、队列、会话、工厂、日志）
> - `docs/linux-architecture.md` — Linux 端详细架构（推流实现、播放端设计、线程模型、错误处理、构建、测试）
> - `docs/timestamp-and-av-sync.md` — 时间戳与音视频同步
> - `docs/milestones.md` — 里程碑拆分
> - `docs/AI_COLLABORATION.md` — 双 AI 协作

## 1. Scope

### Current Version Goals

第一阶段只实现方向 1：

```text
Linux Capture -> Encode -> FLV/RTMP Publish -> SRS
SRS -> Android RTMP Subscribe -> Demux -> Decode -> AV Sync -> Output
```

目标：

- Linux 采集摄像头和麦克风。
- Linux 使用软件编码输出 H.264 + AAC-LC。
- Linux 通过 FFmpeg 完成 FLV/RTMP 封装并推流到 SRS。
- Android 通过 FFmpeg 拉取 RTMP/FLV，解封装、软件解码。
- Android 播放端主要逻辑在 C++：网络、队列、解码、时钟、同步、音频输出、视频渲染。
- 用日志和指标验证首帧时间、端到端延迟、队列水位和 A/V 差值。

### Explicit Non-Goals

第一阶段不做：

- Android 推流到 Linux 的完整链路。
- Linux 播放端完整实现。
- 自研 RTMP Server 或完整手写 RTMP 协议栈。
- WebRTC、自适应码率、多房间、多路转码。
- 硬件编解码、零拷贝、美颜滤镜或复杂 UI。
- 一次性建立完整 Gradle/CMake 工程。本轮只做文档。

### Why Linux To Android First

优先 Linux 推流到 Android，是因为 Linux 端采集和 FFmpeg 工具链更容易快速验证，SRS 和 `ffplay` 能形成清晰的中间验收点。Android 端先聚焦播放链路和 A/V 同步，可以把最难的移动端生命周期、音频主时钟、Surface 重建和 JNI 边界提前暴露出来。

未来方向 2 复用同一套抽象：

- Android 采集适配器实现 `IVideoCapture` / `IAudioCapture`。
- Android MediaCodec 或 FFmpeg 编码器实现 `IVideoEncoder` / `IAudioEncoder`。
- Linux 播放适配器实现 `IVideoRenderer` / `IAudioOutput`。
- `PublishSession` 和 `PlaybackSession` 不绑定平台，双向只是组合不同平台适配器。

## 2. Repository Structure

建议确认后的目录结构：

```text
StreamBridge/
├── AGENTS.md
├── AI_START_PROMPT.md
├── CLAUDE.md
├── README.md
├── docs/
│   ├── AI_COLLABORATION.md
│   ├── architecture.md
│   ├── build-and-run.md
│   ├── milestones.md
│   ├── protocols-and-formats.md
│   ├── timestamp-and-av-sync.md
│   └── decisions/
├── common/
│   ├── include/streambridge/
│   │   ├── media_types.h
│   │   ├── media_errors.h
│   │   ├── media_clock.h
│   │   ├── media_queue.h
│   │   ├── capture.h
│   │   ├── codec.h
│   │   ├── transport.h
│   │   ├── output.h
│   │   └── session.h
│   ├── src/
│   └── tests/
├── linux/
│   ├── app/
│   ├── platform/
│   │   ├── ffmpeg/
│   │   ├── v4l2/
│   │   └── alsa/
│   └── tests/
├── android/
│   ├── app/
│   ├── native/
│   │   ├── jni/
│   │   ├── platform/
│   │   └── playback/
│   └── tests/
├── third_party/
└── scripts/
```

`common/` 只放平台无关的 C++ 抽象、数据结构、队列、状态机、时间基转换、同步算法和软件实现可共享的轻量逻辑。不能包含 Android NDK 类型、JNI 类型、Linux 文件描述符、V4L2/ALSA 头、FFmpeg 具体头文件暴露在公共 API 中。

`linux/` 放 Linux 入口、设备能力探测、FFmpeg avdevice、V4L2/ALSA 后续原生适配、SRS/运行脚本文档和 Linux 平台输出。

`android/` 放 Android 工程、Activity、权限、Surface 生命周期、JNI glue、AAudio/AudioTrack、ANativeWindow/OpenGL ES、MediaCodec 后续适配。

避免过度抽象的规则：

- 只有被两个平台真实复用，或必须保持跨端一致的内容，才进入 `common/`。
- 第一阶段只定义最小可稳定接口，不为未来硬件方案创建空壳实现。
- 平台能力差异通过能力描述和工厂选择表达，不把所有平台选项塞进一个巨型配置类。

## 3. System Data Flow

### Phase 1: Linux Publish, Android Playback

```text
Linux V4L2/ALSA or FFmpeg avdevice
  -> Raw VideoFrame / AudioFrame
  -> Format Convert
  -> H.264 / AAC Encoder
  -> MediaPacket
  -> FLV Mux / RTMP Publisher
  -> SRS
  -> RTMP Subscriber / FLV Demux
  -> MediaPacket
  -> H.264 / AAC Decoder
  -> VideoFrame / AudioFrame
  -> A/V Sync
  -> Android AudioOutput + VideoRenderer
```

Linux 发布端以采集时间戳为源头，把音视频映射到同一单调时间轴。编码后保留 PTS/DTS，封装到 FLV/RTMP 毫秒时间戳。Android 播放端解封装后恢复媒体 PTS，以音频实际播放进度为主时钟决定视频等待、立即渲染或丢帧。

### Future: Android Publish, Linux Playback

```text
Android Camera / AudioRecord
  -> Raw VideoFrame / AudioFrame
  -> Format Convert
  -> MediaCodec or FFmpeg Encoder
  -> MediaPacket
  -> FLV Mux / RTMP Publisher
  -> SRS
  -> Linux RTMP Subscriber / FLV Demux
  -> MediaPacket
  -> Decoder
  -> A/V Sync
  -> Linux AudioOutput + VideoRenderer
```

反向链路复用数据模型、编码器接口、发布订阅接口和同步算法。平台差异限制在 capture、codec adapter、audio output、video renderer 和 lifecycle adapter 中。

## 4. Core Interface Draft

以下为头文件级伪代码，只用于设计讨论，不创建正式代码文件。

### Common Types

```cpp
enum class MediaType { Audio, Video };
enum class CodecId { H264, AAC };
enum class PixelFormat { Unknown, Yuv420p, Nv12, Rgba };
enum class SampleFormat { Unknown, S16, Flt, S16Planar, FltPlanar };

struct TimePointUs {
  int64_t value_us;
};

struct RationalTimeBase {
  int32_t num;
  int32_t den;
};

struct MediaPacket {
  MediaType type;
  CodecId codec;
  std::vector<uint8_t> data;
  int64_t pts_us;
  int64_t dts_us;
  bool is_key_frame;
  RationalTimeBase source_time_base;
};

struct VideoFrame {
  PixelFormat format;
  int width;
  int height;
  int64_t pts_us;
  Plane planes[4];
};

struct AudioFrame {
  SampleFormat format;
  int sample_rate;
  int channels;
  int nb_samples;
  int64_t pts_us;
  Plane planes[8];
};
```

所有跨模块时间统一使用微秒 `_us`。靠近 FFmpeg 或 FLV 边界时可使用 `RationalTimeBase` 或毫秒，但进入 common 队列前必须转换清楚。

### Capture

```cpp
class IVideoCapture {
 public:
  virtual Result<void> open(const VideoCaptureConfig& config) = 0;
  virtual Result<void> start(FrameCallback<VideoFrame> on_frame) = 0;
  virtual void stop() = 0;
  virtual VideoCaptureCapabilities capabilities() const = 0;
};

class IAudioCapture {
 public:
  virtual Result<void> open(const AudioCaptureConfig& config) = 0;
  virtual Result<void> start(FrameCallback<AudioFrame> on_frame) = 0;
  virtual void stop() = 0;
  virtual AudioCaptureCapabilities capabilities() const = 0;
};
```

采集接口由平台实现主动回调 frame。回调不得长期阻塞，实际实现应尽快放入有界队列。采集帧所有权由回调转移给调用方或通过引用计数 buffer 表达。

### Codec

```cpp
class IVideoEncoder {
 public:
  virtual Result<void> open(const VideoEncodeConfig& config) = 0;
  virtual Result<std::vector<MediaPacket>> encode(const VideoFrame& frame) = 0;
  virtual Result<std::vector<MediaPacket>> drain() = 0;
  virtual void close() = 0;
};

class IAudioEncoder { ... };
class IVideoDecoder { ... };
class IAudioDecoder { ... };

struct CodecCapability {
  CodecId codec;
  bool hardware;
  std::vector<PixelFormat> pixel_formats;
  std::vector<SampleFormat> sample_formats;
};
```

FFmpeg、MediaCodec、VAAPI、NVENC、V4L2 M2M 都作为 adapter 实现同一接口。上层只看到能力、配置、输入帧和输出 packet/frame，不依赖具体 API。

### Transport

```cpp
class IMediaPublisher {
 public:
  virtual Result<void> open(const PublishConfig& config) = 0;
  virtual Result<void> write_header(const StreamInfo& info) = 0;
  virtual Result<void> write_packet(const MediaPacket& packet) = 0;
  virtual void close() = 0;
};

class IMediaSubscriber {
 public:
  virtual Result<void> open(const SubscribeConfig& config) = 0;
  virtual Result<StreamInfo> read_header() = 0;
  virtual Result<MediaPacket> read_packet(StopToken stop) = 0;
  virtual void close() = 0;
};
```

第一版发布和订阅由 FFmpeg FLV/RTMP 实现。阻塞读写必须有 interrupt callback 或等价 stop token。

### Output And Sync

```cpp
class IAudioOutput {
 public:
  virtual Result<void> open(const AudioOutputConfig& config) = 0;
  virtual Result<void> start() = 0;
  virtual Result<void> submit(const AudioFrame& frame) = 0;
  virtual int64_t played_position_us() const = 0;
  virtual void flush() = 0;
  virtual void stop() = 0;
};

class IVideoRenderer {
 public:
  virtual Result<void> bind_surface(const NativeSurface& surface) = 0;
  virtual Result<void> render(const VideoFrame& frame) = 0;
  virtual void clear() = 0;
  virtual void unbind_surface() = 0;
};

class IAVSyncController {
 public:
  virtual RenderDecision decide(int64_t video_pts_us,
                                int64_t audio_clock_us,
                                QueueStats stats) = 0;
};
```

音频输出暴露“实际播放进度”，不是仅提交到设备的进度。视频渲染器只接收已经决定应渲染的帧。

## 5. Session Design

### PublishSession

职责：组合采集、编码、封装和网络发布。

```text
Idle -> Preparing -> Running -> Stopping -> Stopped
                    -> Error
```

线程模型：

- control thread：启动、停止、状态迁移；
- video capture thread：产出 raw video；
- audio capture thread：产出 raw audio；
- video encode thread：消费 video queue，输出 video packets；
- audio encode thread：消费 audio queue，输出 audio packets；
- mux/publish thread：按 DTS/PTS 交织写入 RTMP。

### PlaybackSession

职责：组合订阅、解封装、解码、同步和输出。

```text
Idle -> Preparing -> Buffering -> Running -> Stopping -> Stopped
                              -> Reconnecting
                              -> Error
```

线程模型：

- control/JNI thread：接收 start/stop/surface events；
- network demux thread：阻塞读 RTMP，输出 packets；
- video decode thread：输出 decoded video frames；
- audio decode thread：输出 decoded audio frames；
- audio output callback/thread：提交和统计实际播放进度；
- video render thread：根据同步决策等待、渲染或丢帧。

Session 停止必须幂等。`stop()` 先设置 abort 标志，再中断 FFmpeg 阻塞调用，flush 队列，停止输出，join 线程，释放平台资源。

## 6. Queue Rules

所有跨线程队列必须有界：

| 队列 | 数据 | 生产者 | 消费者 | 建议上限 | 满时策略 |
| --- | --- | --- | --- | --- | --- |
| RawVideoQueue | `VideoFrame` | video capture | video encoder | 3-5 帧 | 实时链路丢弃旧视频帧 |
| RawAudioQueue | `AudioFrame` | audio capture | audio encoder | 100-200 ms | 短暂阻塞，超时后报告 underrun/overrun |
| PacketQueue | `MediaPacket` | demux or encoders | decoders or mux | 1-2 秒 | 播放端丢非关键旧视频 packet，音频谨慎 flush |
| DecodedVideoQueue | `VideoFrame` | video decoder | renderer | 3-5 帧 | 丢旧帧，保留最新关键时间附近帧 |
| DecodedAudioQueue | `AudioFrame` | audio decoder | audio output | 200-500 ms | 阻塞或触发 rebuffer |

队列必须支持 `push(timeout)`, `pop(timeout)`, `flush()`, `abort()`, `shutdown()`，并记录水位。

## 7. Android Native Boundary

Android 接收链路尽量放在 C++：

- FFmpeg RTMP subscribe/demux。
- Packet/frame queues。
- 软件解码。
- Audio master clock。
- A/V sync controller。
- Native renderer/audio output adapter。

Kotlin/Java 只负责：

- Activity/Fragment 生命周期；
- 权限申请；
- Surface 创建、变化、销毁；
- 用户输入和配置；
- 传入 RTMP URL；
- 接收少量状态和错误事件。

JNI 边界：

- Java/Kotlin 持有 native handle，不逐帧传递媒体数据。
- `start(url, surface)`、`stop()`、`onSurfaceCreated/Changed/Destroyed()`、`release()` 为稳定 API。
- JNI 层只做参数校验、对象生命周期和线程 attach/detach。
- C++ 需要把 JavaVM 保存为弱平台上下文，禁止长期持有 Activity 强引用。

第一版视频渲染建议：优先 `ANativeWindow`。

| 方案 | 优点 | 缺点 | 建议 |
| --- | --- | --- | --- |
| `ANativeWindow` | 接入快，适合软件解码后直接显示，第一阶段验证成本低 | 色彩转换可能在 CPU，渲染能力有限 | 第一版推荐 |
| OpenGL ES | 更适合 YUV shader、缩放、旋转和后续滤镜 | 工程复杂度高，生命周期和线程更复杂 | 第二阶段或性能优化时引入 |

音频输出建议第一版优先评估 AAudio；若 minSdk 或设备兼容性不足，保留 AudioTrack/Oboe 作为备选。文档和实现必须区分 submitted audio 与 actually played audio。

## 8. Linux Platform Boundary

第一版使用 FFmpeg avdevice 接入 V4L2/ALSA 是合理选择：

- 能快速覆盖采集、格式转换、编码和 RTMP 推流。
- 容易用 ffmpeg/ffprobe/ffplay 对比验证。
- 工作量比同时维护原生 V4L2/ALSA 小。

后续原生 V4L2/ALSA 接入方式：

- 原生 V4L2 adapter 只实现 `IVideoCapture`。
- 原生 ALSA adapter 只实现 `IAudioCapture`。
- 上层 `PublishSession` 不知道采集来源是 FFmpeg avdevice 还是原生 API。

未来 Linux 播放端：

- 视频输出可比较 SDL2、EGL/OpenGL、DRM/KMS。第一版反向链路建议 SDL2，便于音视频输出和窗口管理。
- 音频输出可比较 SDL2 audio、ALSA、PulseAudio/PipeWire。第一版反向链路建议 SDL2 或 ALSA，按部署环境选择。

虚拟机风险：

- USB 摄像头直通不稳定，帧间隔抖动明显。
- 音频设备延迟和时钟可能被宿主机重采样。
- `/dev/video0`、采样率、像素格式不可硬编码，必须做能力探测。
- 虚拟机调度可能放大队列抖动，测试报告需记录运行环境。

## 9. Codec And Format

第一版建议确认：

- 视频：H.264，720p，30 fps，baseline 或 constrained baseline/profile low latency。
- 音频：AAC-LC，48 kHz，双声道。若设备不支持，允许降级到单声道或 44.1 kHz，但必须记录。
- 禁用 B 帧：避免 DTS/PTS 重排序，降低直播延迟和同步复杂度。
- GOP：1-2 秒，即 30-60 帧。
- 码率：720p30 初始 1.5-2.5 Mbps，可按设备能力调整。
- 像素格式：编码前统一到 YUV420P 或 NV12。软件 x264 优先 YUV420P，硬件适配可用 NV12。
- 音频采样格式：编码前统一到 AAC 编码器接受格式，常见为 FLTP。

FLV/RTMP 处理：

- H.264 sequence header 携带 SPS/PPS。
- AAC sequence header 携带 AudioSpecificConfig。
- 关键帧必须可观测并用于重连后恢复。
- RTMP 时间戳为毫秒，公共层内部仍保留微秒，边界处做四舍五入或截断策略并记录。

回退：

- 软件编码器打开失败：报告配置和 FFmpeg 错误，尝试降级分辨率/帧率，不静默切换。
- 未来硬件能力不匹配：能力查询失败或格式不支持时回退软件实现，并记录 `codec_fallback` 日志。

## 10. Build And Dependency Plan

本轮不创建构建文件，只做组织建议。

Android：

- Gradle 管 Android app。
- NDK CMake 构建 `android/native` 与 `common`。
- FFmpeg 建议以预编译 Android ABI 包接入，包来源、版本、configure 参数写入 `docs/build-and-run.md`。
- 禁止提交大型 FFmpeg 二进制到仓库，除非用户明确批准。

Linux：

- CMake 管理 `linux` 与 `common`。
- 第一版优先使用系统安装的 FFmpeg dev 包，或文档化固定版本源码构建流程。
- SRS 外部安装或 Docker 运行，不提交到仓库。
- `third_party/` 只放小型许可证清楚的源码或下载脚本，不放构建产物。

FFmpeg 版本控制：

- Android 和 Linux 文档中记录 FFmpeg major/minor、configure flags、启用库。
- 公共 API 不暴露 FFmpeg 类型，避免 ABI 泄漏。
- 关键封装行为用集成测试验证，而不是假设两端 FFmpeg 完全一致。

## 11. Observability

日志建议字段：

```text
ts_us module session_id thread state event stream pts_us dts_us queue_ms queue_size audio_clock_us video_pts_us av_diff_us latency_us err
```

必须记录：

- 采集时间戳；
- 编码输入/输出 PTS/DTS；
- FLV/RTMP 毫秒时间戳；
- 解封装 packet PTS/DTS；
- 解码 frame PTS；
- 音频提交进度和实际播放进度；
- 视频同步决策；
- 队列水位；
- 首帧时间；
- 重连和资源释放。

测试类型：

- 单元测试：时间基转换、首帧归一化、队列 abort/flush/timeout、状态机、同步决策。
- 集成测试：本地文件推流到 SRS、Linux 设备采集、Android 拉流播放。
- 真机测试：Surface 重建、Activity 旋转/后台、音频设备行为、30 分钟稳定性。

## 12. Single-Side Validation

每一端都必须能在另一端尚未完成时独立验证当前链路。优先使用本地文件、FFmpeg、ffplay、ffprobe 或非常薄的 Python 脚本，怎样简单怎样来。

Android 播放端单边验证：

```text
local media file -> ffmpeg publish -> SRS -> Android subscriber/playback
```

目的：

- 不等待 Linux 推流端完成，也能验证 Android RTMP 拉流、解封装、解码、音频输出、视频渲染和同步。
- 输入优先用本地 mp4/flv 文件，不依赖摄像头或麦克风。
- 推流端可以是 FFmpeg 命令；只有 FFmpeg 命令难以表达测试场景时，才使用 Python 脚本。

Linux 推流端单边验证：

```text
Linux publisher -> SRS -> ffplay / ffprobe
```

目的：

- 不等待 Android 播放端完成，也能验证 Linux 编码、封装、推流、时间戳和重连。
- 采集链路尚未稳定时，可先使用本地文件或 FFmpeg lavfi 测试源模拟摄像头和麦克风。
- 拉流端优先用 `ffplay` 做播放验证，用 `ffprobe` 做格式和时间基检查。

建议模拟输入：

```text
本地文件：sample.mp4
视频测试源：testsrc / smptebars
音频测试源：sine
```

后续如果创建脚本，脚本只能放在 `scripts/` 或测试目录中作为验证工具，不得替代正式业务实现；脚本必须写明输入、输出、依赖、运行命令和预期结果。

## 13. Open Decisions

需要用户后续确认：

1. Android 第一版最低 API 和目标 ABI。
2. Android 第一版音频输出用 AAudio、AudioTrack 还是 Oboe。
3. Android 第一版视频渲染是否确认优先 `ANativeWindow`。
4. Linux 开发环境是物理机、WSL、虚拟机还是远程 Linux。
5. FFmpeg 依赖采用系统包、源码固定版本还是预编译包。
6. SRS 采用本机安装、Docker 还是远程服务。
7. 是否允许在 `scripts/` 中保留最小 FFmpeg/Python 模拟脚本作为端侧验证工具。

## 14. Recommended First Milestone

架构确认后，建议第一个实现里程碑是：

```text
本地文件 -> FFmpeg -> RTMP -> SRS -> ffplay
```

原因：它不依赖真实摄像头、麦克风、Android 真机或 JNI，能先验证 SRS、RTMP URL、H.264/AAC、FLV sequence header 和基础观测日志。
