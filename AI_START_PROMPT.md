# 项目启动提示词

你现在是本项目的首席 C++ 音视频架构师和开发者。请先完整阅读仓库根目录的 `CLAUDE.md`，并严格遵守其中的阶段门禁、范围控制、文档、测试和汇报要求。

## 项目背景

我要开发一个位于同一 Git 仓库中的 Linux + Android 音视频项目，根目录下分别包含 `linux/` 和 `android/`，并允许将真正跨平台的 C++ 逻辑放入 `common/`。

最终需要支持：

1. Linux 采集摄像头和麦克风，软件编码为 H.264/AAC，通过 RTMP 推流；Android 拉流后主要使用 C++ 完成解封装、软件解码、音视频同步、音频播放和视频渲染。
2. Android 采集摄像头和麦克风，通过 RTMP 推流；Linux 拉流、解码、同步并播放。

第一阶段只实现方向 1，但所有核心接口和模块边界必须合理支持未来方向 2。不要现在实现反向链路，只预留必要且克制的扩展点。

第一版先使用软件编解码。Android 以后需要能够接入 MediaCodec 硬件编码和解码；Linux 以后需要能够接入 VAAPI、NVENC、V4L2 M2M 等硬件方案。上层 Session 不应直接依赖某一种编解码器。

RTMP 服务端计划使用 SRS。第一版允许使用 FFmpeg 完成 RTMP/FLV 封装、解封装及软件编解码，不要求手写完整 RTMP 协议栈。

## 本轮唯一任务：整体架构设计

本轮禁止编写业务实现代码。请先检查当前仓库和可用工具链，然后只完成架构设计文档。文档提交后立即停止，等待我评审和确认。

请至少生成：

- `docs/architecture.md`
- `docs/timestamp-and-av-sync.md`
- `docs/milestones.md`
- `docs/AI_COLLABORATION.md`

如确有必要，可以补充少量设计文档，但不要创建 `.cpp`、`.h`、Kotlin/Java 业务代码、Gradle 工程、CMake 实现或占位模块。

## 双 AI 协作方式

本项目将由两个 AI 分别在两台机器上协作推进：

1. Android 端 AI：主要负责 Android 工程、Gradle、JNI/NDK、Android C++ 播放链路、Android 平台 API 边界和真机验证。
2. Linux 端 AI：主要负责 Linux 工程、CMake/Make、Linux 采集推流链路、FFmpeg/Linux 平台 API 边界和 Linux 运行验证。

两个 AI 可以分别分析和实现各自平台，但必须有序协作，不能各自随意修改公共部分。

### 单一事实来源

`docs/AI_COLLABORATION.md` 是两个 AI 的唯一协作事实来源。任何跨端假设如果没有写入该文档，就视为不存在。

本轮创建该文档时，至少包含：

- 当前阶段状态：TODO、IN_PROGRESS、BLOCKED、DONE；
- Android AI 负责范围、Linux AI 负责范围、公共代码范围；
- 禁止各自修改的范围；
- 公共接口约定：数据结构、协议字段、JNI/native bridge、ABI、字节序、字符编码、错误码、版本号；
- 构建约定：Android Gradle/NDK/CMake/ABI/产物路径，Linux 编译器/CMake 或 Make/FFmpeg 依赖/动态库路径/产物路径；
- Decision Log：日期、决策、原因、影响范围、决策人、是否需要另一端确认；
- Pending Changes：待确认的公共接口或构建约定变更；
- Blockers：阻塞项、阻塞端、需要谁处理、需要的信息、临时绕过方案、当前状态；
- 每轮工作结束记录：改了什么、验证了什么、依赖另一端什么、没有改什么、下一步建议。

### 公共部分修改规则

公共部分包括但不限于 `common/`、跨端协议、核心数据结构、公共头文件、接口草案、构建约定和跨端文档。

公共部分必须先设计和记录，再实现。修改顺序为：

1. 在 `docs/AI_COLLABORATION.md` 的 Pending Changes 中提出变更；
2. 说明变更原因、影响 Android 哪些部分、影响 Linux 哪些部分；
3. 等另一端确认，或至少明确记录“待另一端确认”；
4. 再修改公共代码或公共文档；
5. 修改后更新 Decision Log 和对应 Changelog。

本轮只做架构设计，不创建业务代码；因此公共部分只允许形成文档级接口草案和协作约定。

### Git 协作建议

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

如果 Android 端和 Linux 端都需要同一个公共变更，优先在 `shared/interface-contract` 中完成并同步，再分别继续平台端工作。

### 每个 AI 的边界

Android 端 AI 不应随意修改：

- Linux 专属构建脚本；
- Linux main 程序；
- Linux systemd、shell 部署文件；
- 未经协作文档确认的公共协议、公共头文件和核心数据结构。

Linux 端 AI 不应随意修改：

- Android Gradle 配置；
- Android app 代码；
- Android JNI 调用层；
- 未经协作文档确认的公共协议、公共头文件和核心数据结构。

如果某端发现必须修改另一端负责范围，先写入 `docs/AI_COLLABORATION.md` 的 Blockers 或 Pending Changes，不要直接改。

## 架构文档必须回答的问题

### 1. 系统范围

- 当前版本目标和明确非目标是什么？
- 为什么第一阶段选择 Linux 推流到 Android？
- 未来 Android 推流到 Linux 如何复用同一套抽象？

### 2. 仓库和模块结构

- 给出完整目录树以及每个目录的职责。
- 哪些代码属于 `common/`，哪些必须保留在 `linux/` 或 `android/`？
- 如何避免为了跨平台而过度抽象？

### 3. 双向数据流

分别画出并解释：

```text
Linux Capture → Encode → FLV/RTMP Publish → SRS
SRS → Android RTMP Subscribe → Demux → Decode → AV Sync → Output
```

以及未来：

```text
Android Capture → Encode → FLV/RTMP Publish → SRS
SRS → Linux RTMP Subscribe → Demux → Decode → AV Sync → Output
```

### 4. 核心接口草案

请给出重要接口的职责、输入输出、所有权和线程语义，可提供头文件级伪代码，但不得创建正式代码文件。至少评估：

- 视频/音频采集接口；
- 视频/音频编码器和解码器接口；
- 发布端和订阅端接口；
- 音频输出、视频渲染接口；
- MediaPacket、VideoFrame、AudioFrame 数据模型；
- PublishSession、PlaybackSession；
- 编解码器工厂和能力描述；
- 时钟和音视频同步控制器。

解释 FFmpeg 软件实现、Android MediaCodec 和 Linux 硬件实现如何适配这些接口，以及如何避免上层依赖硬件细节。

### 5. Android Native 边界

- Android 拉流、队列、解码、时钟和同步如何主要放在 C++？
- Kotlin/Java 与 JNI 只负责哪些能力？
- Surface、Activity 生命周期、权限、Camera、AudioRecord、AAudio 和 MediaCodec 如何穿过平台边界？
- 如何避免逐帧 JNI 复制和 JNI 生命周期错误？
- 第一版软件解码视频选择 ANativeWindow 还是 OpenGL ES，请比较并推荐。

### 6. Linux 平台边界

- 第一版使用 FFmpeg avdevice 接入 V4L2/ALSA 是否合理？
- 后续原生 V4L2/ALSA 如何接入而不破坏上层？
- 未来 Linux 播放端的视频和音频输出如何设计？
- 虚拟机设备直通、设备能力探测和采集抖动有哪些风险？

### 7. 时间戳和音视频同步

这是重点，请详细设计：

- Linux 和 Android 采集时间戳的来源；
- 不同设备时钟如何映射到统一的单调时间轴；
- 采集时间戳、编码器 PTS/DTS、AVPacket time base、FLV/RTMP 毫秒时间戳、解码帧 PTS、音频设备播放时钟之间如何转换；
- 首帧时间戳如何归一化；
- 为什么使用音频实际播放进度作为主时钟；
- 视频提前、轻微落后和严重落后时分别如何处理；
- 无音频、网络重连、队列清空、Surface 重建时如何重置时钟；
- 摄像头与麦克风长期运行产生时钟漂移时如何检测和补偿；
- 所有时间值的单位、time base 和公式。

### 8. 线程、队列和状态机

- 为 Linux 发布端和 Android 播放端分别给出线程模型。
- 明确每个队列的数据类型、生产者、消费者、容量、背压、丢弃、flush、abort 和 shutdown 行为。
- 明确阻塞 FFmpeg 调用如何中断。
- 给出 PublishSession 和 PlaybackSession 状态机。
- 说明 JNI、音频回调和渲染线程的限制。

### 9. 编解码和格式

第一版建议使用 H.264 + AAC-LC、720p、30 fps、48 kHz。请确认或提出有依据的调整，并说明：

- 为什么第一版禁用 B 帧；
- GOP、码率、profile 和低延迟参数建议；
- 像素格式和采样格式转换放在哪一层；
- SPS/PPS、AudioSpecificConfig 和关键帧在 FLV/RTMP 链路中的处理；
- 软件编解码失败和未来硬件能力不匹配时如何回退。

### 10. 构建与依赖

- Linux 和 Android 如何组织 CMake/Gradle，但本轮不创建构建文件；
- FFmpeg、libx264、SRS、Android NDK、AAudio 等依赖如何管理；
- 哪些依赖应该外部安装，哪些可以作为预编译包，哪些禁止直接提交到仓库；
- 如何保证两端 FFmpeg 版本和 ABI 可控。

### 11. 里程碑和验收

将项目拆成可以独立运行、独立验证的小里程碑。至少包含：

1. 本地文件 RTMP 推流到 SRS，并用 ffplay 验证；
2. Linux 摄像头单视频推流；
3. Linux 音视频采集推流；
4. Android C++ 拉流与软件解码播放；
5. Android 音频主时钟与音视频同步；
6. 重连、资源释放和 30 分钟稳定性测试；
7. Android 推流到 Linux；
8. MediaCodec 和 Linux 硬件编解码扩展。

每个里程碑列出：范围、输入输出、完成标准、测试方法、风险、明确不做的内容。

### 12. 可观测性和测试

- 需要记录哪些时间戳、队列水位、编码/解码耗时和同步指标？
- 如何测量首帧时间、端到端延迟、A/V 差值和长期漂移？
- 哪些模块需要单元测试，哪些需要集成测试或真机测试？
- 给出建议的日志格式和问题定位流程。

## 设计原则

- 以可运行、可验证、可讲清楚为优先，不追求一次做全。
- 不要创建巨型 Manager，不要把平台 API、FFmpeg 和业务状态混在一个类中。
- 不要为了未来硬件编解码写大量空壳代码，只设计稳定边界和最少必要能力。
- 不要把“能播放”当作唯一验收标准；必须能够通过日志和指标验证时间戳与同步。
- 如果仓库已有代码或文档，先阅读并复用，不要覆盖用户已有实现。
- 对没有确定的技术选择，给出 2～3 个方案、优缺点和明确推荐，不要自行假定。

## 本轮最终汇报格式

完成文档后停止，不要进入编码。最后只汇报：

1. 创建或修改了哪些文档；
2. 推荐的总体架构摘要；
3. 最重要的设计取舍；
4. 需要我确认的选项；
5. 架构确认后建议开始的第一个里程碑。

在我明确确认架构之前，不允许继续创建任何业务代码。
