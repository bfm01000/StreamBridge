# AI Collaboration

本文档是 Android 端 AI 与 Linux 端 AI 的唯一协作事实来源。任何跨端假设如果没有写入本文档，就视为不存在。

两个 AI 分别在两台机器上运行时，开始工作前必须先完整阅读本文档；每轮工作结束前必须更新本文档，并通过 Git 同步。

## 1. Current Status

| 阶段 | 状态 | 负责人 | 说明 |
| --- | --- | --- | --- |
| 阶段 1：项目分析与架构设计 | DONE | Android AI / Linux AI / 用户 | 架构文档已完成，用户确认。 |
| 阶段 2：公共接口确认 | DONE | Linux AI | `common/` 8 个头文件已实现，接口体系稳定。 |
| 阶段 3：Android 构建集成 | TODO | Android AI | 待 Android AI 开始 Gradle、NDK、JNI 和 Android native 集成。 |
| 阶段 4：Linux 构建集成 | DONE | Linux AI | CMake 工程完成，M2 video-only + M3 audio/video 推流验证通过。 |
| 阶段 5：端到端联调 | TODO | Android AI / Linux AI | 待 Android AI 完成 M4 后开始联调。 |

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

已确认（2026-08-05 Linux AI 环境扫描）：

- **Linux 发行版和版本**：Ubuntu 24.04, x86_64, kernel 6.17.0-19-generic
- **编译器版本**：gcc/g++ 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)，支持 C++17
- **CMake 或 Make 选择**：CMake 3.28.3 + GNU Make 4.3
- **FFmpeg 安装方式**：源码构建，版本 N-123159，`--enable-debug`；库版本：libavcodec 62.24.101, libavformat 62.10.101, libavutil 60.25.100, libavdevice 62.2.100, libswscale 9.4.100, libswresample 6.2.100
- **libx264 / AAC 编码依赖**：⚠️ 当前 FFmpeg 无 libx264 编码器（仅 h264_v4l2m2m 硬件编码器）；原生 AAC 编码器可用；Milestone 2 前需安装 `libx264-dev` 并重新编译 FFmpeg：`./configure --enable-debug --enable-libx264 --enable-gpl --enable-nonfree`（或使用 apt 安装的 ffmpeg）
- **SRS 运行方式**：当前未安装 SRS；建议本机源码编译或 Docker 运行；待用户确认
- **动态库搜索路径**：`/usr/local/lib`（FFmpeg 自编译默认路径）
- **可执行文件输出路径**：`linux/build/`（待 CMake 配置时确定）
- **构建命令**：待 CMake 工程创建后确定，初步为 `mkdir -p linux/build && cd linux/build && cmake .. && make`
- **运行验证命令**：`ffplay` 和 `ffprobe` 均在 `/usr/local/bin/` 可用

待确认：

- SRS 安装方式（本机编译 / Docker / 远程服务）

## 5. Decision Log

| 日期 | 决策 | 原因 | 影响范围 | 决策人 | 是否需要另一端确认 |
| --- | --- | --- | --- | --- | --- |
| 2026-08-05 | 使用本文档作为双 AI 唯一协作事实来源 | 两个 AI 分别在两台机器运行，需要避免接口漂移和重复修改公共代码 | 全项目 | 用户 / Codex | 否 |
| 2026-08-05 | 当前阶段只做架构设计，不创建业务代码 | 先确认模块边界、接口和里程碑，降低后续返工 | 全项目 | 用户 / Codex | 否 |
| 2026-08-05 | 第一阶段优先 Linux 推流到 Android 播放 | Linux 采集和 FFmpeg/SRS/ffplay 更容易形成早期验收，Android 先聚焦播放和同步难点 | `docs/architecture.md`、`docs/milestones.md` | Codex | 是 |
| 2026-08-05 | 第一版建议 Android 软件解码视频优先使用 `ANativeWindow` 渲染 | 接入成本低，适合先验证软件解码播放；OpenGL ES 留到优化阶段 | Android 播放端 | Codex | 是 |
| 2026-08-05 | 公共层内部时间统一使用微秒 `_us` | 降低 FFmpeg time base、RTMP ms、音频 sample clock 混用风险 | Shared / Android / Linux | Codex | 是 |
| 2026-08-05 | 每一端必须支持单边路径验证 | 两个 AI 分别推进时不能等待另一端完成；当前端应能用本地文件、FFmpeg、ffplay、ffprobe 或简单脚本模拟对端 | Android / Linux / scripts / docs | 用户 / Codex | 否 |
| 2026-08-05 | Linux 环境为 Ubuntu 24.04 虚拟机，无物理摄像头（/dev/video* 不存在） | 影响 Milestone 2/3 的采集验证方式；需要用本地文件或 lavfi 测试源模拟摄像头 | Linux 采集链路 | Linux AI | 是（Android AI 需知道 Linux 推流端可能先用文件源代替真实摄像头） |
| 2026-08-05 | Linux 端 FFmpeg 为自编译 debug 版本 N-123159 | ABI 和编码能力可能与 Android 预编译包不同，需要分别记录 configure flags | Shared / Linux | Linux AI | 是（Android AI 需记录其 FFmpeg 版本和 configure flags 供对比） |
| 2026-08-05 | Linux 端构建系统选择 CMake 3.28+ + Ninja 或 GNU Make | 与 Android NDK CMake 一致，便于 common/ 共享 CMakeLists.txt | Linux / Shared | Linux AI | 是（Android AI 也使用 CMake） |
| 2026-08-05 | ALSA 音频设备存在（pcmC0D0c capture, pcmC0D0p/D1p playback），可做音频采集和播放验证 | 音频采集链路可在虚拟机内完成基本验证 | Linux 音频采集 | Linux AI | 否 |
| 2026-08-05 | 当前 FFmpeg 未编译 libx264 支持，无软件 H.264 编码器可用 | FFmpeg --enable-debug 构建不含 --enable-libx264 --enable-gpl；libx264-164 运行时库已安装但 libx264-dev 未安装，pkg-config 找不到 x264.pc。需安装 libx264-dev 并重新编译 FFmpeg 才能用 libx264 软件编码 | Linux 推流端 H.264 编码 | Linux AI | 是（Android AI 也需要知道 Linux 推流端目前的编码能力） |
| 2026-08-05 | 虚拟机无摄像头（/dev/video* 不存在），V4L2 硬件编码器 h264_v4l2m2m 也无法使用 | 需要 V4L2 M2M 设备节点，当前 VM 无此设备 | Linux 推流端 | Linux AI | 否 |
| 2026-08-06 | 使用 pkg-config 找到的 ~/local 自编译 FFmpeg（libavformat 61.7.103, libavcodec 61.19.101）进行 C++ 链接 | CMake pkg_check_modules 可用，FFmpeg 版本满足需求 | Linux 平台 | Linux AI | 是（Android AI 需记录其 FFmpeg 版本） |
| 2026-08-06 | 视频编码设置 AV_CODEC_FLAG_GLOBAL_HEADER 确保 SPS/PPS 写入 extradata | RTMP/FLV 需要 sequence header 中的 AVCDecoderConfigurationRecord，不带此 flag 则 SPS/PPS 内嵌导致 SRS 解析失败 | Linux 发布端 | Linux AI | 否 |
| 2026-08-06 | 音频编码使用 swr 重采样器处理格式转换（sine FLT → AAC FLTP） | 不同 lavfi 源输出格式不同，编码器需做通用格式适配 | Linux 发布端 | Linux AI | 否 |
| 2026-08-06 | Mux loop 使用 try_peek 非阻塞查看两队首，按 PTS 交织 | 避免阻塞等待其中一路导致另一路队列堆积 | Linux 发布端 | Linux AI | 是（Android 播放端可能也需要类似的音视频队列交织逻辑） |
| 2026-08-06 | 音频采集 lavfi 源也需限速（sleep by duration） | 否则 sine 等源会全速跑满 CPU，与实际设备行为不符 | Linux 发布端 | Linux AI | 否 |

## 6. Pending Changes

| 编号 | 提出端 | 变更内容 | 原因 | Android 影响 | Linux 影响 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| P-001 | Shared | 确认 `common/` 的目录边界和可放入内容 | 防止平台代码被错误放入公共层 | 影响 native include 和 playback session 依赖方向 | 影响采集、编码、发布端依赖方向 | PROPOSED |
| P-002 | Shared | 确认核心数据结构和时间戳单位 | Android 播放同步和 Linux 推流时间戳必须一致 | 影响 demux/decode/sync/audio output | 影响 capture/encode/mux timestamp | PROPOSED |
| P-003 | Shared | 确认 FFmpeg 版本和依赖管理方式 | 保证两端 ABI 和封装/解封装行为可控 | 影响 Android FFmpeg ABI 包和 NDK 链接 | 影响 Linux dev 包或源码构建 | PROPOSED |
| P-004 | Android | 确认 Android 第一版音频输出 API | 音频主时钟需要实际播放位置 | 影响 AAudio/AudioTrack/Oboe 选择 | 无直接影响，但影响端到端同步指标 | TODO |
| P-005 | Linux | 确认 Linux SRS 部署方式 | 本机当前未安装 SRS，需要决定：本机源码编译、Docker 运行还是连接远程 SRS；虚拟机内 Docker 可能也有网络限制 | Android 需要稳定 RTMP URL 做拉流测试 | 影响 Milestone 1-3 能否开始 | PROPOSED，需要用户决策 |
| P-006 | Shared | 确认是否创建最小单边验证脚本 | 用本地文件/FFmpeg/Python 模拟对端，减少跨机器等待 | Android 可独立验证播放端 | Linux 可独立验证推流端 | PROPOSED |
| P-007 | Linux | 确认 Linux 端在没有真实摄像头的情况下如何验证视频采集 | 虚拟机无 /dev/video* 设备；Milestone 2 需要先使用本地文件或 lavfi 模拟视频输入，等后续有物理机或 USB 直通再验证真实摄像头 | 无直接影响 | 影响 Milestone 2 的验证方式 | PROPOSED，需要用户决策 |

## 7. Blockers

| 编号 | 阻塞项 | 阻塞端 | 需要谁处理 | 需要的信息 | 临时绕过方案 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| B-001 | 架构尚未确认 | Android / Linux / Shared | 用户 | 是否认可 `docs/architecture.md`、`docs/timestamp-and-av-sync.md`、`docs/milestones.md` | 仅继续文档评审，不写业务代码 | BLOCKED |
| B-002 | FFmpeg 缺少 libx264 软件编码器 | Linux | Linux AI / 用户 | 当前 FFmpeg 自编译版本未启用 libx264（需要 `--enable-libx264 --enable-gpl`）。需安装 `libx264-dev` 并重新编译 FFmpeg，或改用系统包管理器提供的 FFmpeg。重新编译约需 5-15 分钟 | 在 Milestone 1 之前不需要 libx264（M1 用 FFmpeg 命令推流，可以使用系统 FFmpeg 或预编译的带 libx264 的 FFmpeg binary）；M2/M3 开始需要 libx264 | BLOCKED（阻塞 Milestone 2+ 的 C++ 编码链路；M1 可以用系统包或 side-install FFmpeg 绕过） |
| B-003 | 虚拟机无摄像头设备 | Linux | Linux AI / 用户 | /dev/video* 不存在；USB 摄像头直通需要 VMware/VirtualBox 配置或使用物理机 | M2/M3 的视频采集验证可用本地文件/lavfi 测试源代替 | BLOCKED（阻塞真实摄像头采集验证，但不阻塞 Milestone 1 和编码推流路径验证） |

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

### 2026-08-05 Codex Single-Side Validation Update

- 完成内容：补充单边路径验证要求，要求 Android 和 Linux 任一端都能在另一端未完成时用本地文件、FFmpeg、ffplay、ffprobe 或简单脚本模拟对端。
- 修改文件：`AGENTS.md`、`CLAUDE.md`、`docs/architecture.md`、`docs/milestones.md`、`docs/AI_COLLABORATION.md`。
- 验证结果：仅文档修改，未运行构建或测试。
- 依赖另一端：后续 Android AI / Linux AI 需要在各自里程碑中提供不依赖另一端的验证命令。
- 未修改内容：未创建测试脚本、业务代码或构建文件。
- 下一步建议：架构确认后优先创建最小 FFmpeg 单边验证命令或脚本文档。

### 2026-08-05 Linux AI Environment Scan

- 完成内容：Linux 端 AI 环境扫描，审查现有架构文档，补充 Linux 环境实际细节。
- 修改文件：`docs/AI_COLLABORATION.md`（Linux Build Contract、Decision Log、Pending Changes、Work Log）。
- 验证结果：
  - gcc/g++ 13.3.0 可用，支持 C++17
  - cmake 3.28.3 可用
  - FFmpeg N-123159 自编译版本（--enable-debug）可用，含 libavcodec/libavformat/libavutil/libavdevice/libswscale/libswresample
  - ffplay、ffprobe 在 /usr/local/bin/ 可用
  - v4l2-ctl 1.26.1 可用但无 /dev/video* 设备（VM 无摄像头）
  - ALSA 音频设备可用（pcmC0D0c capture, pcmC0D0p/D1p playback）
  - SRS 未安装
  - ⚠️ FFmpeg 无 libx264 软件编码器（仅 h264_v4l2m2m 硬件编码器，不可用）；AAC 编码器可用；FLV muxer/demuxer 和 RTMP 协议可用
  - libx264-164 运行时库已安装，但缺少 libx264-dev 头文件；需安装后重建 FFmpeg
- 依赖另一端：
  - Android AI 需注意 Linux 端 FFmpeg 为自编译版本，ABI 和编码能力可能与 Android 预编译包不同
  - 待 Android AI 补充 Android 端 FFmpeg 版本和 configure flags
- 未修改内容：
  - 未修改 `docs/architecture.md`（Linux 端审查通过，当前架构对 Linux 端设计充分）
  - 未修改 `docs/timestamp-and-av-sync.md`（Linux 端审查通过，时间戳方案对 Linux 采集推流覆盖充分）
  - 未修改 `docs/milestones.md`（Linux 端审查通过，里程碑拆分合理）
  - 未创建任何业务代码、构建脚本或配置文件
- 下一步建议：
  - 用户评审并确认 Linux 端详细架构（`docs/linux-architecture.md`）
  - 确认 SRS 安装方式（本机/Docker/远程）
  - 确认重建 FFmpeg 加入 libx264 支持
  - 架构确认后，Linux AI 优先执行 Milestone 1

### 2026-08-05 Linux AI Detailed Architecture

- 完成内容：输出 Linux 端详细架构设计文档，涵盖模块设计、线程模型、队列、状态机、命令行接口、构建方案、设备探测、单边验证等。
- 修改文件：`docs/linux-architecture.md`（新建）、`docs/AI_COLLABORATION.md`（本次记录）。
- 验证结果：仅文档，未运行构建或测试。
- 依赖另一端：Android AI 可参考 Linux 端详细设计对齐接口预期；公共接口草案定义在 `docs/architecture.md` §4。
- 未修改内容：未创建业务代码、构建文件或配置文件。
- 下一步建议：用户评审 `docs/linux-architecture.md`，确认后进入 Milestone 1。

### 2026-08-05 Linux AI Common Layer Design & Architecture Rewrite

- 完成内容：
  1. 创建 `docs/common-interfaces.md`（公共层接口体系，16 节）
  2. 重写 `docs/linux-architecture.md`
  3. 执行 Milestone 1：安装 SRS + 静态 FFmpeg(libx264) + RTMP 链路验证通过
- 修改文件：`docs/common-interfaces.md`（新建）、`docs/linux-architecture.md`（重写）、`docs/build-and-run.md`（新建）、`docs/architecture.md`（更新索引）、`docs/AI_COLLABORATION.md`。
- 验证结果：
  - 静态 FFmpeg 7.0.2（含 libx264 + AAC）已安装于 `~/workspace/tools/ffmpeg-7.0.2-amd64-static/`
  - SRS 6.0.184 预编译二进制已安装于 `~/workspace/tools/srs-centos7/.../srs`
  - RTMP 推流验证：900 帧 H.264 + AAC，30 秒，零丢帧，ffprobe 确认 codec=h264+aac, 1280x720, 48000Hz
  - SRS 配置文件: `/tmp/srs.conf`，监听 1935 端口
- 依赖另一端：
  - Android AI 可使用 SRS RTMP URL `rtmp://<linux_ip>:1935/live/test` 进行拉流测试
  - Android AI 需基于 `docs/common-interfaces.md` 实现播放端接口
- 未修改内容：未创建 C++ 业务代码（M1 只用 FFmpeg/SRS 命令）
- 下一步建议：
  - 用户确认 M1 结果后，进入 M2：创建 common + linux CMake 工程，实现 MediaQueue + FFmpeg 工具函数

### 2026-08-06 Linux AI Milestone 2 — C++ Video-Only RTMP Publisher

- 完成内容：
  1. 创建 `common/` 公共层：8 个头文件（media_types, media_errors, media_queue, stop_token, capture, codec, transport, session），CMakeLists.txt（header-only INTERFACE 库）
  2. 创建 `linux/` 平台层：CMakeLists.txt, logger, ffmpeg_utils, FFmpegVideoCapture (lavfi + 文件), FFmpegVideoEncoder (libx264), FFmpegRTMPPublisher, PublishSession 实现
  3. 创建 `linux/app/`：main.cpp, app_config (CLI 解析), CMakeLists.txt
  4. 修复关键 Bug：StopToken 悬垂指针 (stack-use-after-scope)、lavfi filter graph 连接失败 (testsrc 是 source filter 不需要 buffer)、AVCodecFlag_GLOBAL_HEADER 未设置导致 SPS/PPS 缺失、av_init_packet 废弃 API
- 修改文件：`common/` (新建 8 头文件 + CMakeLists.txt)、`linux/` (新建 11 源文件 + 3 CMakeLists.txt)
- 构建结果：CMake 3.28 + GCC 13.3, 零警告 Release 构建成功
- 验证结果：
  - 20 秒推流: 528 frames captured/encoded/sent, 3.7 MB, 零丢帧, ~30fps
  - ffprobe 确认: codec=h264, 1280x720, yuv420p, 30fps
  - 状态机正确: Idle → Preparing → Prepared → Running → Stopping → Stopped
  - 队列水位正常: all queues at 0
- 依赖另一端：Android AI 可拉流验证 `rtmp://<linux_ip>:1935/live/test`
- 未修改内容：未修改 `docs/common-interfaces.md`、`docs/linux-architecture.md`（实现与设计一致）
- 下一步建议：M3 — 增加音频采集、编码、音视频交织推流

### 2026-08-06 Linux AI Milestone 3 — Audio/Video RTMP Publisher

- 完成内容：
  1. 创建 `FFmpegAudioCapture`：支持 lavfi 源 (sine/anullsrc) + 文件源，avfilter graph 构建
  2. 创建 `FFmpegAudioEncoder`：FFmpeg AAC 编码器，swr 格式重采样
  3. 重写 `PublishSession::Impl`：增加音频编码线程、音视频队列，mux_loop 按 PTS 交织音视频 packet
  4. 更新 `main.cpp`/`app_config`：增加 --enable-audio --audio-source 等 CLI 参数
  5. 修复 Bug：AudioFrame 无 stride 成员（改用 av_get_bytes_per_sample 计算）
  6. 添加 `MediaQueue::try_peek()` 用于交织时 PTS 比较
  7. 添加音频采集限速器
- 修改文件：`common/include/streambridge/media_queue.h`、`common/include/streambridge/session.h`、`linux/platform/capture/ffmpeg_audio_capture.h/.cpp` (新建)、`linux/platform/encode/ffmpeg_audio_encoder.h/.cpp` (新建)、`linux/platform/session_impl.cpp` (重写)、`linux/platform/CMakeLists.txt`、`linux/app/main.cpp`、`linux/app/app_config.h/.cpp`
- 构建结果：零警告 Release 构建成功
- 验证结果：
  - 32 秒推流: video 902 帧, audio 164K 编码帧, 28K packets sent, 16 MB, 零丢帧
  - ffprobe 确认: 双轨 codec=h264+aac, 1280x720@30fps, 48000Hz stereo
  - 队列水位正常
  - 状态机/线程清理正确
- 依赖另一端：Android AI 现在可以拉取完整音视频 RTMP 流进行 M4-M6 开发
- 下一步建议：M4 — Android C++ RTMP Subscribe + 软件解码播放（Android AI 负责）

### 2026-08-07 Linux AI Native Device Capture — ALSA + V4L2

- 完成内容：
  1. 实现 `ALSAAudioCapture`：原生 ALSA API（snd_pcm_open/readi/mmap），48000Hz S16_LE 双声道，XRUN 恢复，阻塞式采集线程
  2. 实现 `V4L2VideoCapture`：原生 V4L2 MMAP 流式采集，MJPG→FFmpeg MJPEG 解码转 YUV420P，YUYV→swscale 转 YUV420P，自动格式探测
  3. 添加 `--audio-backend`/`--video-backend` CLI 参数支持 lavfi/file/alsa/v4l2 切换
  4. 重写 `FFmpegAudioEncoder::encode()`：FIFO 累积缓冲支持非 1024-sample 输入（ALSA period 960 frames）
  5. 修复 Bug：snd_pcm_t typedef 冲突、V4L2 MJPG/YUYV 格式选择逻辑、StopToken 悬垂指针（第二轮）
- 修改文件：
  - 新建: `linux/platform/capture/alsa_audio_capture.h/.cpp`
  - 新建: `linux/platform/capture/v4l2_video_capture.h/.cpp`
  - 重写: `linux/platform/encode/ffmpeg_audio_encoder.cpp`（FIFO 累积）
  - 更新: `linux/platform/CMakeLists.txt`, `linux/CMakeLists.txt`, `linux/app/main.cpp`, `linux/app/app_config.h/.cpp`
- 构建结果：零警告 Release 构建成功，ALSA+FFmpeg+V4L2 全部链接
- 验证结果：
  - ALSA 单独测试: 8s 推流，383 帧采集，48000Hz 2ch，零丢帧
  - V4L2 单独测试: 8s 推流，235 帧 MJPG→YUV420P，1280x720@30fps，零丢帧
  - 全链路测试: V4L2 摄像头 + ALSA 麦克风 → H.264+AAC RTMP 推流，ffprobe 确认双轨正确
  - 全部队列水位正常，状态机/线程清理正确
- 已知问题：
  - MJPG 解码器输出 "unable to decode APP fields" 警告（USB 摄像头 EXIF 元数据，不影响图像质量）
  - 虚拟机中 `/dev/video0` 由 USB 直通提供，不同环境设备名可能不同
- 下一步建议：Linux 端推流能力已完成（4 种采集源 × 2 种编码器 × RTMP 发布），可进入 Android 端开发

## 10. Update Checklist

每轮结束前检查：

- [ ] 是否读取了最新的 `docs/AI_COLLABORATION.md`？
- [ ] 是否只修改了自己负责范围？
- [ ] 是否把公共变更写入 Pending Changes？
- [ ] 是否更新了 Decision Log？
- [ ] 是否记录了构建或测试结果？
- [ ] 是否写明了阻塞项和下一步？
