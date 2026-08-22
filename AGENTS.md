# StreamBridge AI 协作规范

本文档适用于所有进入本仓库的 AI 代理。`AGENTS.md` 与 `CLAUDE.md` 必须保持内容完全一致；如需修改规则，必须同时更新两个文件。

## 1. 项目目标

StreamBridge 是一个 Linux + Android 实时音视频学习项目，用于实践 Linux、Android Native、FFmpeg、RTMP、RTP/UDP、编解码、音视频同步与实时传输。

当前项目已经跑通双向 RTMP 音视频链路：

1. Linux Camera + Mic -> 编码 -> RTMP 发送；Android 拉流、解复用、解码、渲染和音频播放。
2. Android Camera + Mic -> 编码 -> RTMP 发送；Linux 拉流、解复用、解码、渲染和音频播放。

下一阶段目标是逐步从“RTMP 双向推流”升级为“实时音视频通话”。当前阶段只处理 **H.264 视频 RTP/UDP 传输**：

```text
Camera
-> VideoEncoder
-> H264 RTP Packetizer
-> UDP
-> H264 RTP Depacketizer
-> VideoDecoder
-> Renderer
```

本阶段不修改音频链路，不引入 WebRTC，不一次性实现完整 RTC 协议栈。RTMP 推流/拉流能力必须继续保留，作为正式可选传输路径和对照验证路径；上层必须能够选择使用 RTMP/FLV 还是 RTP/UDP 视频传输。

## 2. 协作模式

本项目不再要求 Android 端和 Linux 端由两个 AI 分开负责。AI 可以统一阅读、分析和修改全仓库，但必须遵守模块边界和最小改动原则。

允许在同一轮任务中同时修改 Android、Linux、common 和文档，只要这些修改服务于同一个已确认目标，并且不会破坏现有 RTMP 链路或上层传输选择能力。

仍需特别谨慎处理公共接口：

- `common/`、跨端协议、核心数据结构、公共头文件和构建约定属于公共部分；
- 公共接口变更必须先说明影响范围，再实现；
- 如果改动会影响 Android 和 Linux 两端，必须同时考虑两端编译和验证方式；
- 不再需要等待“另一端 AI”确认，但必须在文档或汇报中说明跨端影响。

## 3. 阶段门禁

已确认完成的历史阶段包括：整体架构设计、双向 RTMP 推拉流、Android/Linux 双端播放与推流验证。

当前 RTP/UDP 视频阶段可以开始编码，但必须遵守以下门禁：

1. 每次进入新功能前，先阅读相关现有调用链，不凭猜测改代码。
2. 修改 RTP 传输前，必须确认当前 H.264 输入格式是 Annex-B、AVCC length-prefixed，还是其他格式。
3. 新增 RTP 功能必须小步提交：基础结构、NALU 解析、packetizer、depacketizer、UDP transport、单向接入、双向接入。
4. 每一步尽量能独立编译或用纯单元测试验证。
5. 不得为了跑通 RTP 删除、绕过或破坏 RTMP 现有功能。
6. 不得把 UDP socket、RTP sequence number、SSRC、payload type 等传输细节泄漏到 Camera、Encoder、Decoder、Renderer 等上层业务模块。

如果需求与架构文档冲突，先指出冲突并更新设计方案；不要静默绕过既有架构。

## 4. 单一事实来源

`docs/项目协作与决策日志.md` 仍作为项目阶段、决策和工作记录的事实来源，但其语义从“双 AI 协作记录”调整为“项目协作与决策日志”。

该文档至少维护：

- 当前阶段状态：TODO、IN_PROGRESS、BLOCKED、DONE；
- 当前里程碑目标；
- Android、Linux、Shared 的影响范围；
- 公共接口约定；
- 构建和验证约定；
- Decision Log；
- Pending Changes；
- Blockers；
- 每轮工作结束记录。

公共部分变更流程：

1. 在工作说明或 `docs/项目协作与决策日志.md` 中说明变更动机；
2. 写明对 Android / Linux / common 的影响；
3. 小步修改；
4. 编译或测试验证；
5. 更新 Decision Log 或工作记录。

## 5. 仓库结构

推荐仓库结构如下：

```text
StreamBridge/
├── AGENTS.md
├── CLAUDE.md
├── AI_START_PROMPT.md
├── README.md
├── docs/
│   ├── 项目协作与决策日志.md
│   ├── 总体架构.md
│   ├── 时间戳与音视频同步.md
│   ├── 里程碑.md
│   ├── 构建与运行.md
│   ├── protocols-and-formats.md
│   └── decisions/
├── common/
│   ├── include/
│   ├── src/
│   └── tests/
├── linux/
│   ├── app/
│   ├── player/
│   ├── platform/
│   └── tests/
├── android/
│   ├── app/
│   ├── native/
│   └── tests/
├── third_party/
└── scripts/
```

目录边界：

- `common/` 只放平台无关的 C++ 抽象、媒体数据结构、队列、时间基转换、同步算法、RTP/H.264 纯协议逻辑和可共享工具。
- `common/` 公共头文件不得暴露 Android NDK、JNI、Linux fd、V4L2/ALSA 或 FFmpeg 具体类型。
- `linux/` 保存 Linux 平台入口、V4L2/ALSA/FFmpeg 适配、SDL/ALSA 输出、Linux UDP socket 实现和 Linux 验证程序。
- `android/` 保存 Android 工程、Java UI/权限/Surface 生命周期、JNI 桥接、Android native 播放/推流、AAudio/MediaCodec/ANativeWindow/EGL 实现和 Android UDP socket 实现。
- `third_party/` 不提交大型预编译依赖、构建目录、设备文件或个人绝对路径，除非用户明确批准。

## 6. 架构原则

系统必须按职责拆分，避免把采集、编码、网络、解码、同步和渲染写进单一类。

现有 RTMP 链路：

```text
Capture -> Encode -> FLV/RTMP Publish -> SRS
SRS -> RTMP Subscribe/Demux -> Decode -> Sync -> Render/Play
```

当前 RTP 视频目标链路：

```text
Capture
-> VideoEncoder
-> Encoded H.264 frame / MediaPacket
-> H264RtpPacketizer
-> UDP Sender
-> UDP Receiver
-> RTP Parser
-> H264RtpDepacketizer
-> VideoDecoder
-> Renderer
```

要求：

- Camera、Encoder、Decoder、Renderer 的核心职责不因 RTP 阶段改变。
- RTP 和 RTMP 暂时并存。
- Session 可以选择 RTMP 或 RTP transport，但不直接处理 RTP 包细节。
- 传输层内部管理 sequence number、SSRC、payload type、UDP socket 和 packet loss 统计。
- H.264 RTP packetizer/depacketizer 放在平台无关层；UDP socket 封装放在平台层或薄适配层。
- 使用 RAII 管理 socket、FFmpeg、MediaCodec、AAudio、EGL、ANativeWindow、SDL、ALSA 等资源。
- 线程退出必须可中断、可 join、可重复 stop。

## 7. RTP/UDP 视频阶段范围

本阶段只支持 H.264 over RTP/UDP。

必须实现：

- RTP fixed 12-byte header；
- version、marker、payload type、sequence number、timestamp、SSRC；
- H.264 90000 Hz RTP timestamp，基于当前视频 PTS 转换；
- 默认 UDP payload 约 1200 bytes，避免 IP fragmentation；
- Single NAL Unit Packet；
- FU-A fragmentation；
- FU-A Start/End bit、FU indicator、FU header；
- marker bit 正确表示 access unit / frame 结束；
- Annex-B 和 AVCC length-prefixed 的 H.264 NALU 解析；
- SPS/PPS 缓存，并在 IDR 前发送 SPS/PPS；
- 接收端基本 sequence handling 和小窗口乱序处理；
- FU-A 丢包时丢弃整个损坏 NALU / frame；
- 日志和统计：send packets、receive packets、lost packets、reordered packets、dropped frames、malformed packets。

本阶段不实现：

- RTCP；
- NACK；
- PLI；
- FEC；
- SRTP；
- ICE；
- DTLS；
- congestion control；
- WebRTC；
- 音频 RTP；
- Opus；
- 完整 jitter buffer。

## 8. 编解码策略

当前视频主编码格式为 H.264，音频 RTMP 链路仍使用 AAC-LC。

RTP 视频阶段要求：

- H.264 禁用 B 帧，降低实时延迟和 PTS/DTS 重排复杂度。
- GOP 建议 1-2 秒；RTP 接收端应能通过 IDR + SPS/PPS 恢复。
- 编码器输出进入 RTP 前必须明确格式：Annex-B 或 AVCC length-prefixed。
- 如果是 Annex-B，必须解析 3-byte 和 4-byte start code。
- 如果是 AVCC，必须按 length prefix 拆 NALU。
- 不得直接把 FLV AVCDecoderConfigurationRecord 当作 RTP payload 发送。
- Decoder 期望输入格式必须明确记录；如接收端需要 Annex-B，应由 depacketizer 重建 Annex-B access unit 或符合 decoder 输入约定的 packet。

预留但不提前实现：

- Android MediaCodec 视频编码迁移到 native；
- Linux VAAPI、NVENC、V4L2 M2M；
- 硬件零拷贝路径；
- H.265 RTP；
- 音频 Opus RTP。

## 9. 时间戳与音视频同步

时间戳是项目核心学习目标，不能简化为“能播放就行”。

通用规则：

- 公共层时间统一使用微秒 `_us`。
- RTP H.264 timestamp 使用 90000 Hz clock，变量必须体现 `_rtp_ts` 或明确命名。
- RTP timestamp 必须由视频媒体 PTS 转换，不能随意用系统当前时间替代。
- 如果某端当前视频 PTS 本身来自单调时钟，应说明来源，并在 RTP 层做固定基准归一化。
- 首帧归一化、重连、队列清空和 Surface 重建时必须明确重置规则。

音视频同步规则：

- RTMP 音视频播放路径继续采用音频实际播放进度作为主时钟。
- 当前 RTP 视频阶段不改音频链路；video-only RTP demo 可用视频首帧启动本地视频时钟。
- 后续引入音频 RTP / jitter buffer 前，不提前改变现有音频主时钟设计。

## 10. 线程、队列与生命周期

架构和实现必须明确：

- 采集线程；
- 视频编码线程；
- RTP packetize / UDP send 线程；
- UDP receive / RTP depacketize 线程；
- 视频解码线程；
- 视频渲染线程；
- 音频采集、编码、解码、输出线程；
- 控制线程和 JNI 调用边界。

队列必须：

- 自己持有 mutex 和 condition variable，不向外暴露同步原语；
- 支持容量或时长上限，禁止无限增长；
- 支持阻塞、超时、abort、flush 和安全 shutdown；
- 明确所有权、背压与丢弃策略；
- 记录队列水位。

UDP/RTP 线程必须：

- 支持 stop 请求唤醒阻塞 `recvfrom`；
- stop 顺序清晰：request stop -> shutdown/close socket -> abort queues -> join thread -> release resources；
- 不因单个坏包、乱序包或丢包导致 receiver 崩溃；
- 销毁对象前确保回调不再访问已释放资源。

所有 Session 必须有清晰状态机，例如：

```text
Idle -> Preparing -> Running -> Stopping -> Stopped
                    -> Error
```

## 11. Android 约束

- Android framework 层使用 Java，禁止 Kotlin。
- Java 负责 UI、权限、Activity/Surface 生命周期和少量平台控制。
- 媒体主链路优先放在 C++/native/common 中；当前已存在的 Java MediaCodec 视频编码路径可以保留，小步迁移。
- JNI 只暴露少量稳定 API，禁止不必要的逐帧大对象跨 JNI 复制。
- Surface 生命周期由 Java 管理，再通知 native。
- AAudio、AMediaCodec、ANativeWindow、EGL/GL 资源必须 RAII 或等价封装管理。
- Android RTP/UDP 接入不得破坏现有 RTMP 播放和推流页面。

## 12. Linux 约束

- Linux 端设备能力必须先探测，不硬编码 `/dev/video0`、采样率、声道数或像素格式。
- Linux 采集可以继续使用现有 V4L2/ALSA/FFmpeg 适配。
- Linux 播放可以继续使用现有 SDL renderer 和 ALSA output。
- Linux UDP socket 必须 RAII 封装，支持中断阻塞接收。
- 使用虚拟机时，要记录 USB/音频直通、NAT/桥接网络和调度抖动对 RTP/UDP 的影响。

## 13. 工作流程

每个里程碑遵循：

1. 理解需求；
2. 阅读架构、协作文档和相关现有代码；
3. 列出受影响模块；
4. 设计或确认 API；
5. 给出小步实现计划；
6. 实现；
7. 编写单元测试或集成测试；
8. 构建和运行验证；
9. Code Review；
10. 更新文档和问题记录；
11. 中文汇报结果。

执行规则：

- 用户明确要求可以开始实现时，按计划自主推进，不要停在纯方案。
- 一次只完成当前步骤的目标，不顺手重构无关代码。
- 编译失败必须分析根因并修复本次改动导致的问题。
- 不得用临时 hack 绕过架构问题。
- 不得声称未执行的测试已经通过。
- 遇到外部环境阻塞时，保留现场并给出最小复现命令。

## 14. 代码规范

- C++17；遵循 RAII，避免裸 `new/delete`。
- 明确对象所有权；优先 `std::unique_ptr`，仅在确有共享所有权时使用 `std::shared_ptr`。
- 大媒体 buffer 使用 move 或视图传递，避免不必要 memcpy。
- 需要跨线程持有数据时，必须明确由队列或 owning buffer 接管生命周期。
- 公共头文件不泄漏不必要的平台和 FFmpeg 实现细节。
- 错误返回必须保留上下文，不只返回模糊的 `-1`。
- 日志包含模块、线程、状态、错误码和关键时间戳。
- 禁止忽略 FFmpeg、JNI、AAudio、MediaCodec、socket、EGL、SDL、ALSA 和系统调用返回值。
- 中文注释用于解释设计原因、协议语义、时钟和并发难点，不为显而易见代码逐行翻译。

## 15. 测试与验收

每一端都必须支持单边路径验证。即使另一端尚未完成，当前端也要能用脚本、FFmpeg、本地文件、测试源或 loopback 工具模拟对端行为。

RTMP 既有验收继续保留：

- Android / Linux 双端 RTMP 推流和播放仍可用；
- 上层可以显式选择 RTMP/FLV 或 RTP/UDP 视频传输；
- SRS/RTMP 对照测试仍可运行；
- 音频链路不因 RTP 视频阶段退化。

RTP 视频阶段必须覆盖：

- RTP header 序列化 / 解析单元测试；
- H.264 Annex-B / AVCC NALU 解析单元测试；
- Single NAL packetization；
- FU-A fragmentation 和重组；
- RTP timestamp 90kHz 转换；
- sequence number wraparound 基础处理；
- 小窗口乱序处理；
- FU-A 中间丢包时丢弃损坏 NALU / frame；
- malformed packet 不崩溃；
- UDP sender/receiver stop、shutdown、join；
- Android -> Linux RTP 视频 demo；
- Linux -> Android RTP 视频 demo；
- RTMP 原链路回归验证。

建议持续记录：

- RTP send packets；
- RTP receive packets；
- packet loss；
- reordered packets；
- dropped frames；
- malformed packets；
- keyframe count；
- SPS/PPS resend count；
- encode/decode/render 耗时；
- 端到端视频延迟，如果当前 timestamp 条件允许。

“能够播放”不是完整验收标准，必须提供日志、命令、测试结果或指标证据。

## 16. 范围控制

当前 RTP 视频阶段明确不做：

- WebRTC；
- RTCP；
- NACK / PLI / FEC；
- SRTP / DTLS / ICE；
- congestion control；
- 音频 RTP；
- Opus；
- 完整 jitter buffer；
- 自适应码率；
- 多人或多房间业务；
- 复杂 UI；
- 零拷贝；
- 硬件编解码重构；
- 删除 RTMP 链路；
- 取消上层对 RTMP/FLV 与 RTP/UDP 的传输选择能力。

若发现更优方案，可以提出，但不得未经确认扩大范围。

## 17. 汇报格式

每次工作结束都用中文简洁汇报：

1. 本次完成内容；
2. 修改文件；
3. 构建或测试结果；
4. 发现的问题、风险和阻塞；
5. 下一步建议；
6. 是否需要用户确认。

如果完成了一个实现 Step，还必须补充：

- ownership 变化；
- threading 变化；
- 是否影响 Android / Linux / common；
- 是否影响 RTMP 原链路。

遇到环境或权限阻塞时，保留现场并给出最小复现命令，不要通过删除用户文件或大范围重装规避问题。
