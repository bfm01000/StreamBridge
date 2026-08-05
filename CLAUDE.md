# Linux–Android RTMP 音视频系统协作规范

## 1. 项目目标

本项目用于学习和实践 Linux、FFmpeg、RTMP、Android Native 音视频开发与音视频同步。

最终需要支持两个方向：

1. Linux 采集摄像头和麦克风，经编码后通过 RTMP 推流；Android 拉流、解码、同步并播放。
2. Android 采集摄像头和麦克风，经编码后通过 RTMP 推流；Linux 拉流、解码、同步并播放。

首个可运行版本只实现“Linux 推流 → Android 播放”。架构必须为反向链路预留接口，但在对应里程碑开始前，不得提前实现 Android 推流或 Linux 播放的完整功能。

## 2. 第一原则：先设计，确认后编码

任何业务代码开始前，必须完成整体架构设计并等待用户明确确认。

首次工作只能：

- 检查仓库、工具链和现有文件；
- 澄清会阻塞架构设计的关键问题；
- 创建或更新 `docs/architecture.md`；
- 按第 10 节创建必要的配套设计文档；
- 创建或更新 `docs/AI_COLLABORATION.md`；
- 给出目录规划、接口草案、数据流、线程模型、时间戳方案、里程碑和风险。

首次工作禁止：

- 创建业务实现源文件；
- 编写占位实现、Demo 或“顺便跑通”的代码；
- 下载或编译大型第三方依赖；
- 修改用户环境；
- 在架构未确认前自行进入编码阶段。

文档完成后必须停止，并明确列出：

1. 已确定的设计；
2. 仍需用户决定的问题；
3. 建议优先实现的第一个里程碑；
4. 等待用户确认的内容。

只有用户明确回复“架构确认，可以开始实现”或表达同等含义后，才能编写代码。

## 2.1 双 AI 跨机器协作

本项目可能由两个 AI 分别在两台机器上协作推进：

1. Android 端 AI：主要负责 Android 工程、Gradle、JNI/NDK、Android C++ 播放链路、Android 平台 API 边界和真机验证。
2. Linux 端 AI：主要负责 Linux 工程、CMake/Make、Linux 采集推流链路、FFmpeg/Linux 平台 API 边界和 Linux 运行验证。

两个 AI 可以分别分析和实现各自平台，但必须有序协作，不能各自随意修改公共部分。

`docs/AI_COLLABORATION.md` 是两个 AI 的唯一协作事实来源。任何跨端假设如果没有写入该文档，就视为不存在。

该文档至少维护：

- 当前阶段状态：TODO、IN_PROGRESS、BLOCKED、DONE；
- Android AI 负责范围、Linux AI 负责范围、公共代码范围；
- 禁止各自修改的范围；
- 公共接口约定：数据结构、协议字段、JNI/native bridge、ABI、字节序、字符编码、错误码、版本号；
- 构建约定：Android Gradle/NDK/CMake/ABI/产物路径，Linux 编译器/CMake 或 Make/FFmpeg 依赖/动态库路径/产物路径；
- Decision Log：日期、决策、原因、影响范围、决策人、是否需要另一端确认；
- Pending Changes：待确认的公共接口或构建约定变更；
- Blockers：阻塞项、阻塞端、需要谁处理、需要的信息、临时绕过方案、当前状态；
- 每轮工作结束记录：改了什么、验证了什么、依赖另一端什么、没有改什么、下一步建议。

公共部分包括但不限于 `common/`、跨端协议、核心数据结构、公共头文件、接口草案、构建约定和跨端文档。公共部分必须先记录、再确认、再修改：

1. 在 `docs/AI_COLLABORATION.md` 的 Pending Changes 中提出变更；
2. 说明变更原因、影响 Android 哪些部分、影响 Linux 哪些部分；
3. 等另一端确认，或至少明确记录“待另一端确认”；
4. 再修改公共代码或公共文档；
5. 修改后更新 Decision Log 和对应 Changelog。

两台机器协作时，代码同步必须通过 Git 完成，不要靠手动复制文件。

建议后续分支模型：

- `shared/interface-contract`：公共接口、协议、跨端文档和共享构建约定；
- `android/build-integration`：Android 端工程和平台实现；
- `linux/build-integration`：Linux 端工程和平台实现；
- `integration/e2e-test`：跨端联调和端到端验证。

推荐合并顺序：

```text
shared/interface-contract
→ android/build-integration
→ linux/build-integration
→ integration/e2e-test
```

如果某端发现必须修改另一端负责范围，先写入 `docs/AI_COLLABORATION.md` 的 Blockers 或 Pending Changes，不要直接改。

## 3. 仓库结构

仓库根目录至少采用以下结构，最终以确认后的架构文档为准：

```text
project-root/
├── CLAUDE.md
├── README.md
├── docs/
│   ├── architecture.md
│   ├── protocols-and-formats.md
│   ├── timestamp-and-av-sync.md
│   ├── build-and-run.md
│   ├── milestones.md
│   ├── AI_COLLABORATION.md
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
├── scripts/
└── CMakeLists.txt
```

约束：

- `linux/` 保存 Linux 平台入口、V4L2/ALSA 或 FFmpeg avdevice 适配、渲染和音频输出。
- `android/` 保存 Android 工程、JNI 桥接和 Android 平台实现。
- `common/` 仅保存真正可复用、与平台无关的 C++ 代码；不要为了形式强行共享平台代码。
- Android 的 Kotlin/Java 层应尽量薄，只负责 Activity、权限、生命周期、Surface、Camera/MediaCodec 等系统 API 桥接。
- RTMP 拉流、解封装、队列、软件编解码、时钟和同步等核心逻辑优先放在 C++。

## 4. 总体架构要求

系统必须按职责拆分，避免将采集、编码、网络、解码和渲染写进单一类。

推荐逻辑层次：

```text
Application / Session Orchestrator
        ↓
Capture → Encode → Mux/Publish → RTMP Server
RTMP Server → Subscribe/Demux → Decode → Sync → Render/Play
        ↓
Platform Adapters
```

必须考虑以下核心抽象，具体命名可在设计阶段调整：

- `IVideoCapture`、`IAudioCapture`
- `IVideoEncoder`、`IAudioEncoder`
- `IVideoDecoder`、`IAudioDecoder`
- `IMediaPublisher`、`IMediaSubscriber`
- `IVideoRenderer`、`IAudioOutput`
- `IMediaClock`、`IAVSyncController`
- `MediaPacket`、`VideoFrame`、`AudioFrame`
- `PublishSession`、`PlaybackSession`

设计要求：

- 上层业务依赖抽象，不直接依赖 FFmpeg、MediaCodec 或特定平台 API。
- 软件与硬件编解码器通过相同接口替换，由工厂或配置选择。
- `PublishSession` 与 `PlaybackSession` 分离，避免用大量布尔状态制造双向“大类”。
- 双向传输通过组合两个 Session 实现，不复制完整链路。
- 第一版可使用 FFmpeg 完成 RTMP/FLV 封装和解封装，不要求手写完整 RTMP 协议栈。
- RTMP 服务端优先使用 SRS；客户端不自行实现服务器。

## 5. 编解码策略

第一版：

- 视频：H.264 软件编解码；编码器优先考虑 FFmpeg/libx264，具体依赖需在架构阶段确认。
- 音频：AAC-LC 软件编解码。
- 推荐输入规格：720p、30 fps；48 kHz、双声道，可根据设备能力降级。
- 第一版禁用 B 帧，GOP 建议 1～2 秒，降低实时延迟及时间戳复杂度。

必须预留：

- Android `MediaCodec` 视频硬件编码与解码适配器；
- Linux VAAPI、NVENC、V4L2 M2M 等硬件编解码适配器；
- 像素格式、采样格式和内存类型能力查询；
- 编解码器创建失败时的可控回退策略。

预留接口不等于提前实现。禁止为了“未来扩展”增加没有使用场景的复杂模板、继承层级或空壳模块。

## 6. 时间戳与音视频同步

时间戳是本项目的核心学习目标，设计和实现不得简化为只让画面“看起来能播”。

必须在 `docs/timestamp-and-av-sync.md` 中定义：

- V4L2/ALSA、Android Camera/AudioRecord 的采集时间戳来源；
- 单调时钟的使用方式；
- 视频、音频不同硬件时钟如何映射到统一时间基；
- 编码器 time base、AVPacket PTS/DTS、FLV/RTMP 毫秒时间戳之间的转换；
- 首帧时间戳归一化；
- Android 与 Linux 播放端的主时钟选择；
- 音频设备“已提交”和“已实际播放”的区别；
- 视频等待、立即渲染、丢帧的阈值与策略；
- 无音频、暂停、重连、seek 不适用等异常状态；
- 长时间运行时的音视频时钟漂移和补偿方案。

默认采用音频主时钟：视频同步到音频实际播放时钟。实现前必须给出时钟公式、单位、time base 和线程安全方案。

所有时间变量必须体现单位，例如 `_us`、`_ms`、`_samples`；禁止含义不清的 `timestamp` 在不同模块中混用。

## 7. 线程、队列与生命周期

架构文档必须明确每个平台的线程模型，包括：

- 采集线程；
- 音频和视频编码线程；
- 网络读写或 FFmpeg 阻塞调用线程；
- 音频和视频解码线程；
- 音频回调线程；
- 视频渲染线程；
- 控制线程及 JNI 调用边界。

队列必须：

- 自己持有 mutex 和 condition variable，不向外暴露同步原语；
- 支持容量或时长上限，禁止无限增长；
- 支持阻塞、超时、abort、flush 和安全 shutdown；
- 明确所有权、背压与丢弃策略；
- 记录队列水位，用于定位延迟累积。

所有 Session 必须有清晰状态机，例如：

```text
Idle → Preparing → Running → Stopping → Stopped
                    ↓
                  Error
```

启动失败、网络断开、Activity 销毁和用户主动停止都必须能幂等清理资源，禁止线程泄漏和永久阻塞。

## 8. Android Native 约束

- Android 接收链路的主要逻辑使用 C++ 实现。
- JNI 只暴露少量稳定 API，禁止频繁逐帧跨 JNI 复制数据。
- Surface 生命周期、权限和 Activity 生命周期由 Kotlin/Java 管理，再通知 Native Session。
- 第一版视频软件解码后可以使用 `ANativeWindow` 或 OpenGL ES 渲染，需在架构文档中比较后选择。
- 第一版音频输出优先评估 AAudio；需说明最低 API 与兼容策略。
- 为 MediaCodec 预留输入输出适配，未来可支持 ByteBuffer 和 Surface 模式，但第一版不要实现零拷贝。

## 9. Linux 约束

- 第一版优先通过 FFmpeg avdevice 接入 V4L2 和 ALSA，以控制工作量。
- 架构中保留原生 V4L2/ALSA 适配可能性，但未批准前不重复实现两套采集。
- 播放端视频渲染和音频输出方案必须在架构阶段比较并选择，例如 SDL2 或其他轻量方案。
- 所有设备能力必须先探测，不硬编码 `/dev/video0`、采样率、声道数或像素格式。
- 使用虚拟机时，要记录 USB/音频直通和调度抖动可能造成的影响。

## 10. 文档先行

架构阶段至少输出：

### `docs/architecture.md`

- 目标与非目标；
- 系统上下文和双向数据流图；
- 目录结构；
- 模块职责与依赖关系；
- 核心接口草案；
- Linux/Android 平台适配边界；
- 发布链路和播放链路；
- 软件/硬件编解码扩展方式；
- 线程模型与状态机；
- 错误处理、资源释放和重连边界；
- 关键设计取舍与待确认项。

### `docs/timestamp-and-av-sync.md`

- 完整时间戳流转；
- 时钟定义与同步算法；
- 队列和渲染决策；
- 漂移处理；
- 可观测指标与测试方法。

### `docs/milestones.md`

至少拆为：

1. 本地文件经 FFmpeg 推到 SRS，再由 ffplay 验证；
2. Linux 摄像头单视频推流；
3. Linux 摄像头和麦克风音视频推流；
4. Android C++ 拉流、软件解码和播放；
5. Android 音频主时钟与音视频同步；
6. 稳定性、重连、指标和长时间漂移测试；
7. Android 推流到 Linux；
8. Android MediaCodec 与 Linux 硬件编解码扩展。

每个里程碑必须列出范围、验收标准、测试方法、已知风险和明确不做的内容。

## 11. 工作流程

每个里程碑遵循：

1. 理解需求；
2. 阅读架构和相关现有代码；
3. 列出受影响模块；
4. 设计或确认 API；
5. 给出小步实现计划；
6. 实现；
7. 编写单元测试或集成测试；
8. 构建和运行验证；
9. Code Review；
10. 更新文档和问题记录；
11. 停止并汇报，等待下一个指令。

不得跳过 Review。不得一次实现多个尚未确认的里程碑。

如果需求与架构文档冲突，先指出冲突并提出文档修改方案，不得静默绕过架构。

## 12. 代码规范

- C++17；遵循 RAII，避免裸 `new/delete`。
- 明确对象所有权；优先 `std::unique_ptr`，仅在确有共享所有权时使用 `std::shared_ptr`。
- FFmpeg 对象使用自定义 deleter 或小型 RAII 封装。
- 公共头文件不泄漏不必要的平台和 FFmpeg 实现细节。
- 错误返回必须保留上下文，不只返回模糊的 `-1`。
- 日志包含模块、线程、状态、错误码和关键时间戳。
- 禁止忽略 FFmpeg、JNI、AAudio、MediaCodec 和系统调用的返回值。
- 不提交大型预编译依赖、构建目录、设备文件或个人绝对路径。
- 中文注释用于解释设计原因、协议语义、时钟和并发难点，不为显而易见代码逐行翻译。

## 13. 测试和验收

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
- `audio_clock`、`video_pts` 和 `av_diff`；
- 丢帧数、重连次数和错误原因。

“能够播放”不是完整验收标准，必须提供日志、命令、测试结果或指标证据。

## 14. 范围控制

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

## 15. AI 汇报格式

每次工作结束都用中文简洁汇报：

1. 本次完成内容；
2. 修改文件；
3. 构建/测试结果；
4. 发现的问题和风险；
5. 下一步建议；
6. 是否需要用户确认。

不得声称未实际执行的测试已经通过。遇到环境或权限阻塞时，保留现场并给出最小复现命令，不要通过删除用户文件或大范围重装规避问题。
