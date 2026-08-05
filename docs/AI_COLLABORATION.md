# AI Collaboration

本文档是 Android 端 AI 与 Linux 端 AI 的唯一协作事实来源。任何跨端假设如果没有写入本文档，就视为不存在。

两个 AI 分别在两台机器上运行时，开始工作前必须先完整阅读本文档；每轮工作结束前必须更新本文档，并通过 Git 同步。

## 1. Current Status

| 阶段 | 状态 | 负责人 | 说明 |
| --- | --- | --- | --- |
| 阶段 1：项目分析与架构设计 | BLOCKED | Android AI / Linux AI / 用户 | 架构文档已生成，等待用户评审确认；确认前不允许业务代码。 |
| 阶段 2：公共接口确认 | TODO | Shared | 先确认 `common/` 边界、数据结构、协议和构建约定。 |
| 阶段 3：Android 构建集成 | TODO | Android AI | 架构确认后再开始 Gradle、NDK、JNI 和 Android native 集成。 |
| 阶段 4：Linux 构建集成 | TODO | Linux AI | 架构确认后再开始 CMake/Make、FFmpeg、V4L2/ALSA 和 Linux 可执行程序。 |
| 阶段 5：端到端联调 | TODO | Android AI / Linux AI | 两端基于已确认公共接口做联调验证。 |

状态枚举：

- TODO：尚未开始；
- IN_PROGRESS：正在进行；
- BLOCKED：被依赖、环境或决策阻塞；
- DONE：已完成并同步。

## 2. Role Ownership

### Android AI

主要负责：

- Android 工程结构；
- Gradle 配置；
- NDK / CMake 集成；
- JNI 边界；
- Android C++ 播放链路；
- Surface、Activity 生命周期、权限、AAudio、AudioRecord、Camera、MediaCodec 适配边界；
- Android 真机或模拟器验证。

未经确认，不应直接修改：

- Linux 专属构建脚本；
- Linux main 程序；
- Linux systemd、shell 部署文件；
- 公共协议、公共头文件和核心数据结构。

### Linux AI

主要负责：

- Linux 工程结构；
- CMake / Make / shell 构建脚本；
- Linux 采集推流链路；
- FFmpeg、V4L2、ALSA、SRS 相关运行验证；
- Linux 平台 API 边界；
- Linux 可执行程序、日志和部署说明。

未经确认，不应直接修改：

- Android Gradle 配置；
- Android app 代码；
- Android JNI 调用层；
- 公共协议、公共头文件和核心数据结构。

### Shared

公共部分包括：

- `common/`；
- 跨端协议；
- 核心数据结构；
- 公共头文件；
- 接口草案；
- 构建约定；
- 跨端文档。

公共部分必须先记录、再确认、再修改。

## 3. Shared Interface Contract

当前处于架构设计阶段，以下内容均为待确认项。

### Data Models

待确认：

- `MediaPacket`
- `VideoFrame`
- `AudioFrame`
- 时间戳单位和 time base
- 所有权模型
- 队列传递语义

### Core Interfaces

待确认：

- `IVideoCapture`
- `IAudioCapture`
- `IVideoEncoder`
- `IAudioEncoder`
- `IVideoDecoder`
- `IAudioDecoder`
- `IMediaPublisher`
- `IMediaSubscriber`
- `IVideoRenderer`
- `IAudioOutput`
- `IMediaClock`
- `IAVSyncController`
- `PublishSession`
- `PlaybackSession`

### ABI And Format Rules

待确认：

- C++ 标准和编译器要求；
- Android ABI 列表；
- Linux 目标架构；
- FFmpeg 版本范围；
- H.264 profile、GOP、B 帧策略；
- AAC-LC 采样率、声道数、采样格式；
- 字节序；
- 字符编码；
- 错误码模型；
- 日志字段。

## 4. Build Contract

### Android

待确认：

- Gradle 版本；
- Android Gradle Plugin 版本；
- NDK 版本；
- CMake 版本；
- minSdk / targetSdk；
- ABI 列表；
- native 库输出路径；
- FFmpeg 依赖方式；
- 构建命令；
- 运行验证命令。

### Linux

待确认：

- Linux 发行版和版本；
- 编译器版本；
- CMake 或 Make 选择；
- FFmpeg 安装方式；
- libx264 / AAC 编码依赖；
- SRS 运行方式；
- 动态库搜索路径；
- 可执行文件输出路径；
- 构建命令；
- 运行验证命令。

## 5. Decision Log

| 日期 | 决策 | 原因 | 影响范围 | 决策人 | 是否需要另一端确认 |
| --- | --- | --- | --- | --- | --- |
| 2026-08-05 | 使用本文档作为双 AI 唯一协作事实来源 | 两个 AI 分别在两台机器运行，需要避免接口漂移和重复修改公共代码 | 全项目 | 用户 / Codex | 否 |
| 2026-08-05 | 当前阶段只做架构设计，不创建业务代码 | 先确认模块边界、接口和里程碑，降低后续返工 | 全项目 | 用户 / Codex | 否 |
| 2026-08-05 | 第一阶段优先 Linux 推流到 Android 播放 | Linux 采集和 FFmpeg/SRS/ffplay 更容易形成早期验收，Android 先聚焦播放和同步难点 | `docs/architecture.md`、`docs/milestones.md` | Codex | 是 |
| 2026-08-05 | 第一版建议 Android 软件解码视频优先使用 `ANativeWindow` 渲染 | 接入成本低，适合先验证软件解码播放；OpenGL ES 留到优化阶段 | Android 播放端 | Codex | 是 |
| 2026-08-05 | 公共层内部时间统一使用微秒 `_us` | 降低 FFmpeg time base、RTMP ms、音频 sample clock 混用风险 | Shared / Android / Linux | Codex | 是 |

## 6. Pending Changes

| 编号 | 提出端 | 变更内容 | 原因 | Android 影响 | Linux 影响 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| P-001 | Shared | 确认 `common/` 的目录边界和可放入内容 | 防止平台代码被错误放入公共层 | 影响 native include 和 playback session 依赖方向 | 影响采集、编码、发布端依赖方向 | PROPOSED |
| P-002 | Shared | 确认核心数据结构和时间戳单位 | Android 播放同步和 Linux 推流时间戳必须一致 | 影响 demux/decode/sync/audio output | 影响 capture/encode/mux timestamp | PROPOSED |
| P-003 | Shared | 确认 FFmpeg 版本和依赖管理方式 | 保证两端 ABI 和封装/解封装行为可控 | 影响 Android FFmpeg ABI 包和 NDK 链接 | 影响 Linux dev 包或源码构建 | PROPOSED |
| P-004 | Android | 确认 Android 第一版音频输出 API | 音频主时钟需要实际播放位置 | 影响 AAudio/AudioTrack/Oboe 选择 | 无直接影响，但影响端到端同步指标 | TODO |
| P-005 | Linux | 确认 Linux 运行环境和 SRS 部署方式 | 采集设备、FFmpeg 依赖和 RTMP 验证依赖环境 | Android 需要稳定 RTMP URL 做拉流测试 | 影响 Milestone 1-3 | TODO |

## 7. Blockers

| 编号 | 阻塞项 | 阻塞端 | 需要谁处理 | 需要的信息 | 临时绕过方案 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| B-001 | 架构尚未确认 | Android / Linux / Shared | 用户 | 是否认可 `docs/architecture.md`、`docs/timestamp-and-av-sync.md`、`docs/milestones.md` | 仅继续文档评审，不写业务代码 | BLOCKED |

## 8. Git Workflow

两台机器协作时必须通过 Git 同步，不要手动复制文件。

建议分支：

- `shared/interface-contract`
- `android/build-integration`
- `linux/build-integration`
- `integration/e2e-test`

推荐合并顺序：

```text
shared/interface-contract
-> android/build-integration
-> linux/build-integration
-> integration/e2e-test
```

公共接口优先在 `shared/interface-contract` 中稳定，再分别进入 Android 和 Linux 分支。

## 9. Work Log

每个 AI 每轮工作结束前，在本节追加记录。

### 2026-08-05 Codex

- 完成内容：创建双 AI 协作初始文档。
- 修改文件：`docs/AI_COLLABORATION.md`。
- 验证结果：仅文档修改，未运行构建或测试。
- 依赖另一端：Android AI 和 Linux AI 启动前需先阅读本文档。
- 未修改内容：未创建业务代码、构建脚本或平台工程。
- 下一步建议：先完成 `docs/architecture.md`、`docs/timestamp-and-av-sync.md`、`docs/milestones.md`，再由用户确认架构。

### 2026-08-05 Codex Architecture Pass

- 完成内容：根据 `AI_START_PROMPT.md` 完成整体架构设计、时间戳与音视频同步设计、里程碑拆分。
- 修改文件：`docs/architecture.md`、`docs/timestamp-and-av-sync.md`、`docs/milestones.md`、`docs/AI_COLLABORATION.md`。
- 验证结果：仅文档修改，未运行构建或测试；当前仓库尚无业务源码和构建文件。
- 依赖另一端：Android AI 和 Linux AI 需要基于这些文档评审各自平台边界，公共接口确认前不得实现业务代码。
- 未修改内容：未创建 `.cpp`、`.h`、Kotlin/Java、Gradle、CMake、Make、shell 业务实现。
- 下一步建议：用户先评审并确认架构；确认后优先执行 Milestone 1，本地文件推流到 SRS 并用 ffplay 验证。

## 10. Update Checklist

每轮结束前检查：

- [ ] 是否读取了最新的 `docs/AI_COLLABORATION.md`？
- [ ] 是否只修改了自己负责范围？
- [ ] 是否把公共变更写入 Pending Changes？
- [ ] 是否更新了 Decision Log？
- [ ] 是否记录了构建或测试结果？
- [ ] 是否写明了阻塞项和下一步？
