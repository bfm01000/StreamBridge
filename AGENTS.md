# StreamBridge AI 协作规约

本仓库是 Linux + Android RTMP 音视频学习项目。所有 AI 代理进入仓库后，必须先阅读 `CLAUDE.md` 和 `AI_START_PROMPT.md`，并以它们作为最高优先级的项目规则。

## 当前阶段

当前阶段默认是架构设计阶段。除非用户明确确认“架构确认，可以开始实现”或表达同等含义，否则禁止创建业务实现代码。

允许：

- 阅读仓库和工具链；
- 创建或更新设计文档；
- 梳理目录、模块、接口草案、线程模型、时间戳方案和里程碑；
- 更新协作文档。

禁止：

- 创建 `.cpp`、`.h`、Kotlin/Java 业务代码；
- 创建 Gradle、CMake、Make 等实际工程实现；
- 编写占位 Demo；
- 下载或编译大型第三方依赖；
- 擅自扩大项目范围。

## 双 AI 分工

本项目可能由两个 AI 分别在两台机器上协作：

- Android 端 AI：负责 Android 工程、Gradle、JNI/NDK、Android C++ 播放链路、Android 平台 API 边界和真机验证。
- Linux 端 AI：负责 Linux 工程、CMake/Make、Linux 采集推流链路、FFmpeg/Linux 平台 API 边界和 Linux 运行验证。

两个 AI 都可以阅读全仓库，但不要随意修改对方负责范围。

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

## 单一事实来源

`docs/AI_COLLABORATION.md` 是两个 AI 的唯一协作事实来源。任何跨端假设如果没有写入该文档，就视为不存在。

该文档至少维护：

- 当前阶段状态；
- Android / Linux / common 的负责范围；
- 公共接口约定；
- 构建约定；
- Decision Log；
- Pending Changes；
- Blockers；
- 每轮工作结束记录。

## 公共部分规则

公共部分包括 `common/`、跨端协议、核心数据结构、公共头文件、接口草案、构建约定和跨端文档。

公共部分必须先记录、再确认、再修改：

1. 在 `docs/AI_COLLABORATION.md` 的 Pending Changes 中提出变更；
2. 写明原因和对 Android / Linux 的影响；
3. 等另一端确认，或明确记录“待另一端确认”；
4. 再修改公共代码或公共文档；
5. 修改后更新 Decision Log。

## Git 协作

两台机器协作时，必须通过 Git 同步，不要手动复制文件。

建议分支：

- `shared/interface-contract`
- `android/build-integration`
- `linux/build-integration`
- `integration/e2e-test`

公共接口优先在 `shared/interface-contract` 中稳定，再分别进入 Android 和 Linux 分支。

## 汇报要求

每次结束工作时，用中文简洁汇报：

1. 本次完成内容；
2. 修改文件；
3. 构建或测试结果；
4. 风险和阻塞；
5. 下一步建议；
6. 是否需要用户确认。

不得声称没有执行过的测试已经通过。
