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

如确有必要，可以补充少量设计文档，但不要创建 `.cpp`、`.h`、Kotlin/Java 业务代码、Gradle 工程、CMake 实现或占位模块。

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
