# StreamBridge AI 协作规范

本文档适用于所有进入本仓库的 AI 代理。`AGENTS.md` 与 `CLAUDE.md` 必须保持内容完全一致；如需修改规则，必须同时更新两个文件。

## 1. 项目目标

StreamBridge 是一个 Linux + Android RTMP 音视频学习项目，用于实践 Linux、FFmpeg、RTMP、Android Native 音视频开发与音视频同步。

最终支持两个方向：

1. Linux 采集摄像头和麦克风，编码后通过 RTMP 推流；Android 拉流、解码、同步并播放。
2. Android 采集摄像头和麦克风，编码后通过 RTMP 推流；Linux 拉流、解码、同步并播放。

首个可运行版本只实现“Linux 推流 -> Android 播放”。架构必须为反向链路预留边界，但在对应里程碑开始前，不得提前实现 Android 推流或 Linux 播放的完整功能。

## 2. 阶段门禁

任何业务代码开始前，必须先完成整体架构设计并等待用户明确确认。

架构确认前只能：

- 检查仓库、工具链和现有文件；
- 澄清会阻塞架构设计的关键问题；
- 创建或更新 `docs/architecture.md`；
- 创建或更新 `docs/timestamp-and-av-sync.md`；
- 创建或更新 `docs/milestones.md`；
- 创建或更新 `docs/AI_COLLABORATION.md`；
- 给出目录规划、接口草案、数据流、线程模型、时间戳方案、里程碑和风险。

架构确认前禁止：

- 创建 `.cpp`、`.h`、Kotlin/Java 业务代码；
- 创建 Gradle、CMake、Make 等实际工程实现；
- 编写占位实现、Demo 或“顺便跑通”的代码；
- 下载或编译大型第三方依赖；
- 修改用户环境；
- 自行进入编码阶段。

只有用户明确回复“架构确认，可以开始实现”或表达同等含义后，才能编写代码。

## 3. 双 AI 跨机器协作

本项目可能由两个 AI 分别在两台机器上协作：

- Android 端 AI：负责 Android 工程、Gradle、JNI/NDK、Android C++ 播放链路、Android 平台 API 边界和真机验证。
- Linux 端 AI：负责 Linux 工程、CMake/Make、Linux 采集推流链路、FFmpeg/Linux 平台 API 边界和 Linux 运行验证。

两个 AI 都可以阅读全仓库，但不能随意修改对方负责范围。

Android 端 AI 不应直接修改：

- Linux 专属构建脚本；
- Linux main 程序；
- Linux systemd、shell 部署文件；
- 未经确认的公共协议、公共头文件和核心数据结构。

Linux 端 AI 不应直接修改：

- Android Gradle 配置；
- Android app 代码；
- Android JNI 调用层；
- 未经确认的公共协议、公共头文件和核心数据结构。

## 4. 单一事实来源

`docs/AI_COLLABORATION.md` 是两个 AI 的唯一协作事实来源。任何跨端假设如果没有写入该文档，就视为不存在。

该文档至少维护：

- 当前阶段状态：TODO、IN_PROGRESS、BLOCKED、DONE；
- Android AI、Linux AI、Shared 的负责范围；
- 禁止各自修改的范围；
- 公共接口约定；
- 构建约定；
- Decision Log；
- Pending Changes；
- Blockers；
- 每轮工作结束记录。

公共部分包括 `common/`、跨端协议、核心数据结构、公共头文件、接口草案、构建约定和跨端文档。

公共部分必须先记录、再确认、再修改：

1. 在 `docs/AI_COLLABORATION.md` 的 Pending Changes 中提出变更；
2. 写明原因和对 Android / Linux 的影响；
3. 等另一端确认，或明确记录“待另一端确认”；
4. 再修改公共代码或公共文档；
5. 修改后更新 Decision Log。

两台机器协作时必须通过 Git 同步，不要手动复制文件。建议分支：

- `shared/interface-contract`
- `android/build-integration`
- `linux/build-integration`
- `integration/e2e-test`

公共接口优先在 `shared/interface-contract` 中稳定，再分别进入 Android 和 Linux 分支。

## 5. 仓库结构

确认后的仓库结构建议如下：

```text
StreamBridge/
├── AGENTS.md
├── CLAUDE.md
├── AI_START_PROMPT.md
├── README.md
├── docs/
│   ├── AI_COLLABORATION.md
│   ├── architecture.md
│   ├── timestamp-and-av-sync.md
│   ├── milestones.md
│   ├── build-and-run.md
│   ├── protocols-and-formats.md
│   └── decisions/
├── common/
│   ├── include/
│   ├── src/
│   └── tests/
├── linux/
│   ├── app/
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

- `common/` 只放真正平台无关的 C++ 抽象、数据结构、队列、状态机、时间基转换、同步算法和可共享工具。
- `linux/` 保存 Linux 平台入口、V4L2/ALSA 或 FFmpeg avdevice 适配、Linux 端渲染和音频输出。
- `android/` 保存 Android 工程、JNI 桥接、Activity/Surface 生命周期和 Android 平台实现；Android 端禁止使用 Kotlin。
- `third_party/` 不提交大型预编译依赖、构建目录、设备文件或个人绝对路径。

## 6. 架构原则

系统必须按职责拆分，避免把采集、编码、网络、解码、同步和渲染写进单一类。

推荐逻辑层次：

```text
Application / Session Orchestrator
        ↓
Capture -> Encode -> Mux/Publish -> RTMP Server
RTMP Server -> Subscribe/Demux -> Decode -> Sync -> Render/Play
        ↓
Platform Adapters
```

核心抽象至少考虑：

- `IVideoCapture`、`IAudioCapture`
- `IVideoEncoder`、`IAudioEncoder`
- `IVideoDecoder`、`IAudioDecoder`
- `IMediaPublisher`、`IMediaSubscriber`
- `IVideoRenderer`、`IAudioOutput`
- `IMediaClock`、`IAVSyncController`
- `MediaPacket`、`VideoFrame`、`AudioFrame`
- `PublishSession`、`PlaybackSession`

要求：

- 上层业务依赖抽象，不直接依赖 FFmpeg、MediaCodec 或特定平台 API。
- `PublishSession` 与 `PlaybackSession` 分离，避免双向“大类”。
- 双向传输通过组合两个 Session 实现，不复制完整链路。
- 第一版可使用 FFmpeg 完成 RTMP/FLV 封装、解封装及软件编解码，不要求手写完整 RTMP 协议栈。
- RTMP 服务端优先使用 SRS，客户端不自行实现服务器。

## 7. 编解码策略

第一版：

- 视频：H.264 软件编解码，优先 FFmpeg/libx264。
- 音频：AAC-LC 软件编解码。
- 推荐输入规格：720p、30 fps、48 kHz，双声道；可根据设备能力降级。
- 禁用 B 帧，GOP 建议 1-2 秒，降低实时延迟和时间戳复杂度。

必须预留：

- Android MediaCodec 编码和解码适配器；
- Linux VAAPI、NVENC、V4L2 M2M 等硬件编解码适配器；
- 像素格式、采样格式和内存类型能力查询；
- 编解码器创建失败时的可控回退策略。

预留接口不等于提前实现。禁止为了“未来扩展”增加没有使用场景的复杂模板、继承层级或空壳模块。

## 8. 时间戳与音视频同步

时间戳是本项目核心学习目标，不能简化为只让画面“看起来能播”。

必须在 `docs/timestamp-and-av-sync.md` 中定义：

- Linux V4L2/ALSA、Android Camera/AudioRecord 的采集时间戳来源；
- 单调时钟使用方式；
- 不同设备时钟如何映射到统一时间基；
- 采集时间戳、编码器 PTS/DTS、AVPacket time base、FLV/RTMP 毫秒时间戳、解码帧 PTS、音频设备播放时钟之间如何转换；
- 首帧时间戳归一化；
- 为什么使用音频实际播放进度作为主时钟；
- 视频等待、立即渲染、丢帧阈值；
- 无音频、网络重连、队列清空、Surface 重建时如何重置时钟；
- 长时间运行时的音视频漂移检测和补偿方案。

默认采用音频主时钟：视频同步到音频实际播放时钟。所有时间变量必须体现单位，例如 `_us`、`_ms`、`_samples`；禁止含义不清的 `timestamp` 在不同模块中混用。

## 9. 线程、队列与生命周期

架构和实现必须明确：

- 采集线程；
- 音频和视频编码线程；
- 网络读写或 FFmpeg 阻塞调用线程；
- 音频和视频解码线程；
- 音频回调线程；
- 视频渲染线程；
- 控制线程和 JNI 调用边界。

队列必须：

- 自己持有 mutex 和 condition variable，不向外暴露同步原语；
- 支持容量或时长上限，禁止无限增长；
- 支持阻塞、超时、abort、flush 和安全 shutdown；
- 明确所有权、背压与丢弃策略；
- 记录队列水位。

所有 Session 必须有清晰状态机，例如：

```text
Idle -> Preparing -> Running -> Stopping -> Stopped
                    -> Error
```

启动失败、网络断开、Activity 销毁和用户主动停止都必须能幂等清理资源，禁止线程泄漏和永久阻塞。

## 10. Android Native 约束

- Android 接收链路主要逻辑使用 C++ 实现。
- JNI 只暴露少量稳定 API，禁止逐帧跨 JNI 复制媒体数据。
- Android 端禁止使用 Kotlin。需要 Android framework 层时使用 Java；业务链路、媒体处理、队列、解码、同步和渲染控制优先放在 C++。
- Surface 生命周期、权限和 Activity 生命周期由 Java 管理，再通知 Native Session。
- 第一版视频软件解码后可以使用 `ANativeWindow` 或 OpenGL ES 渲染，需在架构文档中比较后选择。
- 第一版音频输出优先评估 AAudio；需说明最低 API 和兼容策略。
- 为 MediaCodec 预留输入输出适配，未来可支持 ByteBuffer 和 Surface 模式，但第一版不要实现零拷贝。

## 11. Linux 约束

- 第一版优先通过 FFmpeg avdevice 接入 V4L2 和 ALSA，以控制工作量。
- 架构中保留原生 V4L2/ALSA 适配可能性，但未批准前不重复实现两套采集。
- 播放端视频渲染和音频输出方案必须在架构阶段比较并选择。
- 所有设备能力必须先探测，不硬编码 `/dev/video0`、采样率、声道数或像素格式。
- 使用虚拟机时，要记录 USB/音频直通和调度抖动可能造成的影响。

## 12. 工作流程

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
11. 停止并汇报，等待下一个指令。

不得跳过 Review。不得一次实现多个尚未确认的里程碑。如果需求与架构文档冲突，先指出冲突并提出文档修改方案，不得静默绕过架构。

## 13. 代码规范

- C++17；遵循 RAII，避免裸 `new/delete`。
- 明确对象所有权；优先 `std::unique_ptr`，仅在确有共享所有权时使用 `std::shared_ptr`。
- FFmpeg 对象使用自定义 deleter 或小型 RAII 封装。
- 公共头文件不泄漏不必要的平台和 FFmpeg 实现细节。
- 错误返回必须保留上下文，不只返回模糊的 `-1`。
- 日志包含模块、线程、状态、错误码和关键时间戳。
- 禁止忽略 FFmpeg、JNI、AAudio、MediaCodec 和系统调用的返回值。
- 中文注释用于解释设计原因、协议语义、时钟和并发难点，不为显而易见代码逐行翻译。

## 14. 测试与验收

每一端都必须支持单边路径验证。即使另一端尚未完成，当前端也要能用脚本、FFmpeg、Python 或本地文件模拟对端行为，优先选择最简单可复现方案。

单边验证要求：

- Android 播放端未等 Linux 推流端完成前，可以用本地媒体文件通过 FFmpeg 推 RTMP 到 SRS，模拟 Linux 推流端。
- Linux 推流端未等 Android 播放端完成前，可以用 `ffplay`、`ffprobe` 或简单脚本模拟 Android 拉流端。
- Linux 采集链路可先不用摄像头和麦克风，使用本地文件或 FFmpeg lavfi 测试源验证编码、封装、推流和时间戳。
- 优先使用 FFmpeg 命令；如果 FFmpeg 难以表达，再考虑 Python 脚本。脚本只用于测试和模拟，不替代正式业务实现。
- 每个端侧里程碑必须给出“不依赖另一端”的验证命令、输入文件、预期输出和失败排查方式。

必须覆盖：

- 时间基转换和首帧归一化单元测试；
- 队列 abort、flush、timeout、容量上限和并发退出；
- Session 状态迁移与重复 stop；
- 音视频同步决策；
- 编解码器工厂的软件实现选择与失败回退；
- 断网、服务端不可达、输入设备拔出和 Surface 销毁；
- 连续运行 30 分钟后的队列水位、内存、端到端延迟和 A/V 差值。

建议持续记录：

- 首帧时间；
- 端到端延迟；
- 编码和解码耗时；
- 音视频队列长度；
- `audio_clock_us`、`video_pts_us`、`av_diff_us`；
- 丢帧数、重连次数和错误原因。

“能够播放”不是完整验收标准，必须提供日志、命令、测试结果或指标证据。不得声称未实际执行的测试已经通过。

## 15. 范围控制

第一版明确不做：

- 自研 RTMP Server；
- 完整手写 RTMP 协议栈；
- WebRTC；
- 自适应码率；
- 多路转码；
- 美颜与复杂滤镜；
- 零拷贝；
- 硬件编解码实现；
- 多人或多房间业务；
- 复杂 UI。

若发现更优方案，可以提出，但不得未经确认扩大范围。

## 16. 汇报格式

每次工作结束都用中文简洁汇报：

1. 本次完成内容；
2. 修改文件；
3. 构建或测试结果；
4. 发现的问题、风险和阻塞；
5. 下一步建议；
6. 是否需要用户确认。

遇到环境或权限阻塞时，保留现场并给出最小复现命令，不要通过删除用户文件或大范围重装规避问题。
