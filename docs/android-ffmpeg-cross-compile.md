# Android FFmpeg 交叉编译：从概念到实战

本文档从零开始讲解如何为 Android 平台编译 FFmpeg 动态库，涵盖交叉编译的基本概念、
NDK 工具链的使用、FFmpeg 编译配置，以及最终集成到 Android 项目中的完整流程。

---

## 目录

1. [什么是交叉编译](#1-什么是交叉编译)
2. [核心概念：Host、Target、Toolchain、Sysroot](#2-核心概念)
3. [Android NDK 交叉编译工具链详解](#3-android-ndk-交叉编译工具链)
4. [FFmpeg 编译系统概述](#4-ffmpeg-编译系统概述)
5. [实战：构建 FFmpeg for Android arm64-v8a](#5-实战构建)
6. [构建产物的目录结构](#6-构建产物)
7. [集成到 Android 项目](#7-集成到-android-项目)
8. [常见问题与调试](#8-常见问题与调试)
9. [总结](#9-总结)

---

## 1. 什么是交叉编译

### 1.1 本地编译 vs 交叉编译

**本地编译 (Native Compilation)**：在目标平台上编译，生成在目标平台上运行的程序。

```text
x86_64 Linux 机器 → gcc/clang → x86_64 Linux 可执行文件
   (编译平台)                    (运行平台)

   同一个平台！
```

**交叉编译 (Cross-Compilation)**：在一个平台上编译，生成在另一个不同平台上运行的程序。

```text
x86_64 Windows 机器 → clang (交叉模式) → aarch64 Android .so 文件
   (Host 平台)                              (Target 平台)

   不同的 CPU 架构 + 不同的操作系统！
```

### 1.2 为什么需要交叉编译？

对于 Android 开发，我们需要交叉编译的原因很直接：

| 因素 | 开发者机器 (Host) | Android 手机 (Target) |
|------|-------------------|----------------------|
| CPU 架构 | x86_64 (Intel/AMD) | arm64-v8a (ARM Cortex-A) |
| 操作系统 | Windows/Linux/macOS | Android (Linux 内核 + Bionic libc) |
| 指令集 | x86-64 (CISC) | AArch64 (RISC) |
| C 运行时 | glibc / MSVC / libc++ | Bionic libc |

**手机上的 CPU 和电脑上的 CPU 说不同的"语言"**。x86_64 的机器码在 ARM 处理器上完全无法执行。
所以必须用一个能在 x86_64 上运行、但能生成 ARM 机器码的编译器——这就是交叉编译器。

### 1.3 交叉编译的三要素

每次交叉编译都涉及三个关键角色：

```text
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   Build 机器     │────►│   Host 机器      │────►│   Target 机器    │
│   (构建平台)     │     │   (编译器运行平台) │     │   (程序运行平台)  │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

对我们来说：
- **Build = Host**：都是 x86_64 Windows（编译器也在 Windows 上运行）
- **Target**：aarch64 Android（编译出的程序在 Android 手机上运行）

> **术语约定**：在 GNU Autotools 和 FFmpeg 中，这三个角色称为 `--build`、`--host`、`--target`。
> 大多数交叉编译场景（包括我们的）中 `build == host`，`target` 是目标平台。

---

## 2. 核心概念

### 2.1 Host 与 Target

```text
Host (编译器运行的机器)          Target (编译产物运行的机器)
┌──────────────────────┐        ┌──────────────────────────┐
│ x86_64 Windows       │        │ aarch64 Android          │
│                      │        │                          │
│ 编译器: clang        │ ──►    │ 原生代码: .so 文件        │
│ 运行在: Windows      │  生成  │ 运行在: Android/Linux     │
│ 输出: ARM64 机器码   │        │ Bionic libc              │
└──────────────────────┘        └──────────────────────────┘
```

### 2.2 Toolchain（工具链）

工具链不仅仅是编译器，它是**一组工具的集合**：

```text
Toolchain 组成:
┌─────────────────────────────────────────────────────────┐
│ 编译器 (Compiler)                                        │
│   clang / gcc                                            │
│   负责: C/C++ 源码 → 目标平台汇编 → 目标平台机器码        │
├─────────────────────────────────────────────────────────┤
│ 汇编器 (Assembler)                                       │
│   as / llvm-as                                           │
│   负责: 汇编代码 → 目标平台机器码 (object file)           │
├─────────────────────────────────────────────────────────┤
│ 链接器 (Linker)                                          │
│   ld / lld                                               │
│   负责: 多个 .o 文件 + 库 → 最终 .so / 可执行文件        │
├─────────────────────────────────────────────────────────┤
│ 归档器 (Archiver)                                        │
│   ar / llvm-ar                                           │
│   负责: .o 文件 → .a 静态库                              │
├─────────────────────────────────────────────────────────┤
│ 其他工具                                                 │
│   ranlib   — 为静态库生成索引                            │
│   strip    — 去掉调试符号以减小体积                       │
│   nm       — 列出目标文件中的符号                        │
│   objdump  — 反汇编和查看目标文件信息                     │
└─────────────────────────────────────────────────────────┘
```

**为什么需要专用工具链？** 因为每个平台的二进制格式不同：

| 平台 | 可执行格式 | 动态库格式 | 机器码 |
|------|-----------|-----------|--------|
| Windows x86_64 | PE32+ (.exe) | .dll | x86-64 |
| Linux x86_64 | ELF64 | .so | x86-64 |
| Android arm64 | ELF64 | .so | AArch64 |

虽然 Android 和 Linux 都使用 ELF 格式和 `.so` 后缀，但**机器码完全不同**！
x86_64 的 ELF 包含 x86 指令，ARM64 的 ELF 包含 ARM 指令——绝对不能混用。

### 2.3 Sysroot（系统根目录）

Sysroot 是**目标平台的"迷你系统目录"**，包含编译时需要的一切：

```text
NDK Sysroot 的结构:
toolchains/llvm/prebuilt/windows-x86_64/sysroot/
├── usr/
│   ├── include/          # ★ 目标平台的头文件
│   │   ├── stdio.h       #    Bionic libc 的 stdio.h
│   │   ├── stdlib.h
│   │   ├── pthread.h     #    Android 的 pthread 实现
│   │   ├── android/      #    Android 特有 API
│   │   │   ├── native_window.h   # ANativeWindow
│   │   │   ├── aaudio/AAudio.h   # AAudio
│   │   │   └── log.h             # __android_log_print
│   │   └── ...
│   └── lib/              # ★ 目标平台的库文件
│       └── aarch64-linux-android/
│           └── 21/       # API level 21
│               ├── libc.so
│               ├── libm.so
│               ├── libdl.so
│               ├── liblog.so
│               ├── libandroid.so
│               └── ...
```

**Sysroot 的作用**：

```text
编译器视角:
┌─────────────────────────────────────────────────────┐
│ gcc/clang 编译 hello.c 时:                          │
│                                                     │
│ #include <stdio.h>  ← 从 sysroot/usr/include/ 找    │
│ #include <android/log.h> ← 从 sysroot/usr/include/ 找│
│                                                     │
│ 链接时:                                             │
│ -lc → 找 libc.so → sysroot/usr/lib/arm64/21/       │
│ -lm → 找 libm.so → sysroot/usr/lib/arm64/21/       │
│                                                     │
│ 如果不设 sysroot，编译器会用 Host 的头文件和库，     │
│ 那就变成 x86_64 的本地编译了，编译产物无法在         │
│ Android 上运行！                                    │
└─────────────────────────────────────────────────────┘
```

### 2.4 ABI（Application Binary Interface）

ABI 决定了编译好的二进制文件如何与系统交互。Android 支持的主要 ABI：

| ABI | CPU 架构 | 代表设备 |
|-----|---------|---------|
| arm64-v8a | ARM 64-bit (AArch64) | 几乎所有 2016+ 的手机 |
| armeabi-v7a | ARM 32-bit | 老旧/低端设备 |
| x86_64 | Intel 64-bit | 模拟器 |
| x86 | Intel 32-bit | 模拟器 |

**我们的选择**：只编译 `arm64-v8a`，因为：
- 几乎所有现代 Android 手机都是 ARM64
- 减少构建时间
- 减少 APK 体积
- 如果需要模拟器或老旧设备，可以后续添加

API Level 选择 21 (Android 5.0)：
- 覆盖 99%+ 的设备
- 提供足够的 API 支持（AAudio 需要 API 26+，但我们会在运行时检查）

---

## 3. Android NDK 交叉编译工具链

### 3.1 NDK 工具链的布局

NDK r28 的工具链位于：

```text
$NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/
├── bin/
│   ├── aarch64-linux-android21-clang      # ARM64 API 21 C 编译器 (shell 脚本)
│   ├── aarch64-linux-android21-clang.cmd  # 同上 (Windows CMD 版本)
│   ├── aarch64-linux-android21-clang++    # ARM64 API 21 C++ 编译器
│   ├── aarch64-linux-android21-clang++.cmd
│   ├── aarch64-linux-android26-clang      # ARM64 API 26 C 编译器
│   ├── aarch64-linux-android26-clang++
│   ├── ... (其他 ABI 和 API level 的组合)
│   ├── llvm-ar        # 归档器 (所有 target 共用)
│   ├── llvm-as        # 汇编器
│   ├── llvm-ranlib    # 静态库索引生成
│   ├── llvm-strip     # 符号剥离
│   ├── llvm-nm        # 符号查看
│   └── ld.lld         # 链接器
└── sysroot/
    └── usr/
        ├── include/   # Bionic libc + Android API 头文件
        └── lib/
            └── aarch64-linux-android/
                └── 21/  # API level 21 的库文件
                    ├── libc.so
                    ├── libm.so
                    └── ...
```

### 3.2 编译器命名规则

NDK 的 clang 包装器遵循严格的命名：

```text
<架构>-linux-android<API level>-clang
   │       │       │            │
   │       │       │            └── 编译器类型 (clang 或 clang++)
   │       │       └── 目标 API level
   │       └── 目标操作系统 (Linux kernel + Android)
   └── CPU 架构 (aarch64 = ARM 64-bit)
```

示例：
- `aarch64-linux-android21-clang`：生成 ARM64 代码，至少运行在 Android 5.0 (API 21)
- `armv7a-linux-androideabi21-clang`：生成 ARM32 代码
- `x86_64-linux-android21-clang`：生成 x86_64 代码（模拟器用）

**为什么要带 API level？** 因为不同 Android 版本的系统库 API 不同。用 API 21 编译的代码可以在 API 21+ 上运行，但不能使用 API 26 才引入的函数（如 AAudio 某些高级功能）。

### 3.3 交叉编译器的实际行为

当你执行 `aarch64-linux-android21-clang hello.c -o hello` 时：

```text
1. clang 以 "目标 = aarch64-linux-android21" 模式启动
2. 自动设置 sysroot = $NDK/toolchains/llvm/prebuilt/.../sysroot
3. #include <stdio.h> → 读取 Bionic libc 的 stdio.h (不是 Windows 的)
4. 编译生成 ARM64 机器码 (不是 x86_64)
5. 链接 Bionic libc (不是 glibc，不是 MSVCRT)
6. 输出 ARM64 ELF 格式的 hello (在 Windows 上无法执行！)
```

**验证交叉编译结果**：

```bash
# 查看文件类型
$ file hello
hello: ELF 64-bit LSB shared object, ARM aarch64, ...

# 查看动态链接依赖
$ llvm-readelf -d hello | grep NEEDED
0x0000000000000001 (NEEDED)    Shared library: [libc.so]

# 注意：是 libc.so (Bionic)，不是 libc.so.6 (glibc) 也不是 msvcrt.dll
```

---

## 4. FFmpeg 编译系统概述

### 4.1 FFmpeg 的 configure/make 构建流程

FFmpeg 使用经典的 GNU Autotools 风格构建系统：

```text
源码下载
   │
   ▼
./configure [选项...]   ← 检测系统能力，生成 config.h 和 config.mak
   │
   ▼
make                    ← 编译所有 .c → .o
   │
   ▼
make install            ← 安装库/头文件到 --prefix 指定目录
```

### 4.2 configure 做了什么

FFmpeg 的 `configure` 是一个大型 shell 脚本 (约 8000 行)，它做的事情包括：

```text
1. 检测编译器: gcc? clang? 什么版本? 支持哪些 flags?
2. 检测目标平台: OS? CPU? 大小端?
3. 检测依赖库: x264? x265? vpx? 在哪个路径?
4. 检测指令集: NEON? SSE? AVX? (对交叉编译很重要)
5. 生成:
   - config.h    — C 头文件，定义 HAVE_*, ENABLE_* 等宏
   - config.mak  — Makefile 包含文件，定义编译选项
   - config.asm  — 汇编相关宏
```

### 4.3 交叉编译时需要告诉 configure 的关键信息

```text
┌────────────────────┬──────────────────────────────────┐
│ configure 选项      │ 作用                             │
├────────────────────┼──────────────────────────────────┤
│ --enable-cross-    │ ★ 告诉 configure: 不要运行编译出  │
│   compile          │   的测试程序 (它们在 Host 上跑不了)│
├────────────────────┼──────────────────────────────────┤
│ --target-os=android│ 目标操作系统 (影响系统调用、       │
│                    │ 线程实现、网络 API 的选择)         │
├────────────────────┼──────────────────────────────────┤
│ --arch=aarch64     │ 目标 CPU 架构                     │
│ --cpu=armv8-a      │ 具体 CPU 型号 (影响指令集优化)     │
├────────────────────┼──────────────────────────────────┤
│ --cc=<编译器路径>   │ ★ 指定交叉编译器                  │
│ --cxx=<C++编译器>   │                                   │
│ --ar=<归档器>       │                                   │
│ --ranlib=<索引器>   │                                   │
├────────────────────┼──────────────────────────────────┤
│ --cross-prefix=    │ ★ 另一种指定编译器的方式:         │
│   aarch64-...-     │   ${cross_prefix}gcc              │
│                    │   ${cross_prefix}ar               │
│                    │   (NDK clang 不遵循 gcc 命名，     │
│                    │    所以我们不用这个，直接用 --cc)   │
├────────────────────┼──────────────────────────────────┤
│ --sysroot=<路径>    │ sysroot 路径 (NDK clang 已经内置， │
│                    │ 所以一般不显式指定)                │
├────────────────────┼──────────────────────────────────┤
│ --enable-shared    │ 生成 .so 动态库                    │
│ --disable-static   │ 不生成 .a 静态库                   │
├────────────────────┼──────────────────────────────────┤
│ --disable-asm      │ ★ 禁用汇编优化 (交叉编译时如果     │
│                    │   遇到汇编错误，先加这个让编译通过) │
├────────────────────┼──────────────────────────────────┤
│ --extra-cflags=    │ 额外的 C 编译参数                  │
│ --extra-ldflags=   │ 额外的链接参数                     │
└────────────────────┴──────────────────────────────────┘
```

### 4.4 FFmpeg 的模块化组件

FFmpeg 由多个库组成，每个库负责不同的功能：

```text
libavformat   — 封装/解封装 (FLV, MP4, RTMP, HLS, ...)
libavcodec    — 编解码器 (H.264, H.265, AAC, MP3, ...)
libavutil     — 工具函数 (内存管理, 数学, 日志, ...)
libswscale    — 图像缩放和像素格式转换
libswresample — 音频重采样和格式转换
libavfilter   — 音视频滤镜
libavdevice   — 采集设备 (摄像头, 麦克风, 屏幕采集)
libpostproc   — 后处理
```

**对于 Android 播放端，我们只需要前 5 个库的最小功能子集：**

```text
我们的需求:
├── libavformat  → 只需要 FLV 解封装 + RTMP 协议
├── libavcodec   → 只需要 H.264 解码 + AAC 解码
├── libavutil    → 总是需要 (其他库的依赖)
├── libswscale   → 需要 (YUV → RGBA 转换)
└── libswresample → 需要 (FLTP → S16 转换给 AAudio)

不需要:
├── libavfilter  ✗ 不做滤镜
├── libavdevice  ✗ 不做采集 (那是 Linux 推流端的活)
├── libpostproc  ✗ 不做后处理
├── 所有编码器    ✗ 播放端只需要解码
├── 所有封装器    ✗ 播放端不需要封装输出
└── 命令行工具    ✗ (ffmpeg, ffplay, ffprobe)
```

**精简编译的原因**：完整 FFmpeg 的 `.so` 文件可能超过 100MB，精简后可以控制在 10-20MB。

---

## 5. 实战构建

### 5.1 前提条件

需要以下工具：

| 工具 | 用途 | 如何获取 |
|------|------|---------|
| Android NDK r28+ | 交叉编译工具链 | Android Studio SDK Manager 或手动下载 |
| Git Bash (Windows) | 运行 shell 脚本 | Git for Windows 自带 |
| curl | 下载 FFmpeg 源码 | Git Bash 自带 |
| tar | 解压源码 | Git Bash 自带 |
| make.exe | 执行 Makefile | NDK 自带: `$NDK/prebuilt/windows-x86_64/bin/make.exe` |

### 5.2 构建脚本完整解析

我们项目的构建脚本位于 `scripts/build-ffmpeg-android.sh`。以下逐节解释：

#### 5.2.1 环境变量设置

```bash
#!/bin/bash
set -euo pipefail  # 遇到错误立即退出

# === 可配置参数 ===
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0.2}"  # FFmpeg 版本
API="${API:-21}"                            # 目标 Android API level
NDK_ROOT="${ANDROID_NDK_HOME:-D:/soft/AS_sdk/ndk/28.2.13676358}"

# === 路径推导 ===
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
THIRD_PARTY_DIR="${PROJECT_DIR}/third_party"
FFMPEG_SRC_DIR="${THIRD_PARTY_DIR}/ffmpeg-${FFMPEG_VERSION}"
BUILD_OUTPUT="${THIRD_PARTY_DIR}/ffmpeg-android/arm64-v8a"

# === NDK 工具链位置 ===
TOOLCHAIN="${NDK_ROOT}/toolchains/llvm/prebuilt/windows-x86_64"
MAKE="${NDK_ROOT}/prebuilt/windows-x86_64/bin/make.exe"
TARGET="aarch64-linux-android"
```

**关键点解释**：

- `TARGET="aarch64-linux-android"`：这被称为 "target triple"，它是 GNU 构建系统的标准格式：`<arch>-<vendor>-<os>-<abi>`
  - `aarch64`：ARM 64位体系结构
  - `linux`：内核类型（Android 使用 Linux 内核）
  - `android`：操作系统（Android 特指，不是普通 Linux）

- API level 影响哪些 Android API 可以在编译后的代码中使用。选 21 是因为它覆盖了几乎所有设备，同时提供了足够的 POSIX 兼容性。

#### 5.2.2 下载 FFmpeg 源码

```bash
if [ ! -d "${FFMPEG_SRC_DIR}" ]; then
    echo "=== Downloading FFmpeg ${FFMPEG_VERSION} ==="
    cd "${THIRD_PARTY_DIR}"
    FFMPEG_TAR="ffmpeg-${FFMPEG_VERSION}.tar.xz"
    if [ ! -f "${FFMPEG_TAR}" ]; then
        curl -L --retry 3 -o "${FFMPEG_TAR}" \
            "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
    fi
    echo "=== Extracting FFmpeg ==="
    tar xf "${FFMPEG_TAR}"
fi
```

**为什么选择 FFmpeg 7.0.2？**
- 这是 2024 年的稳定版本，与 Linux 推流端使用的版本兼容
- 支持 Android 交叉编译
- 7.x 系列包含最新的编解码器优化

**`.tar.xz` vs `.tar.gz`**：xz 压缩率更高，下载更快（约 10MB vs 15MB）。

#### 5.2.3 设置交叉编译工具链

```bash
export PATH="${TOOLCHAIN}/bin:${PATH}"

CC="${TARGET}${API}-clang"        # aarch64-linux-android21-clang
CXX="${TARGET}${API}-clang++"     # aarch64-linux-android21-clang++

export AR="${TOOLCHAIN}/bin/llvm-ar"
export AS="${TOOLCHAIN}/bin/llvm-as"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
export NM="${TOOLCHAIN}/bin/llvm-nm"
```

**为什么 AR/AS/RANLIB/STRIP/NM 不需要 target 前缀？**

因为 LLVM 工具是**通用的**——它们读取 ELF 文件头来判断目标架构：

```bash
# llvm-ar 自动识别 .o 文件的架构
$ llvm-ar rcs libfoo.a foo.o    # foo.o 是什么架构，.a 就是什么架构

# 而传统的 GNU binutils 需要为每个架构编译不同的版本:
# aarch64-linux-android-ar  (ARM64 专用)
# x86_64-linux-gnu-ar       (x86_64 专用)
```

#### 5.2.4 configure 配置

```bash
./configure \
    --prefix="${BUILD_OUTPUT}" \
    --enable-cross-compile \
    --target-os=android \
    --arch=aarch64 \
    --cpu=armv8-a \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --enable-shared \
    --disable-static \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-encoders \
    --disable-muxers \
    --disable-devices \
    --disable-filters \
    --enable-swscale \
    --enable-swresample \
    --enable-protocol=rtmp \
    --enable-protocol=file \
    --enable-demuxer=flv,live_flv \
    --enable-decoder=h264,aac \
    --enable-parser=h264,aac \
    --disable-asm \
    --extra-cflags="-O2 -fPIC -DANDROID"
```

**各选项逐个解释**：

```text
--prefix=<path>
  make install 时把文件安装到哪。编译产物会放到这个目录下。

--enable-cross-compile
  ★ 最重要的交叉编译开关。告诉 configure:
  - 不要试图运行编译出的测试程序 (它们在 Host 上跑不了)
  - 许多功能检测从"编译+运行"变为"仅编译"或"查表"

--target-os=android
  告诉 FFmpeg:
  - 使用 Bionic libc (不是 glibc)
  - 使用 Android 的 pthread 实现
  - 使用 Android 的网络栈
  - 影响很多底层 #ifdef

--arch=aarch64
  目标 CPU 架构。影响:
  - 选择哪些汇编优化文件参与编译
  - 内存对齐等底层参数

--cpu=armv8-a
  具体 CPU 型号。armv8-a 是 ARM64 的基线，所有 ARM64 手机都支持。
  如果指定 cortex-a76 等具体型号，可能在某些手机上无法运行。

--cc / --cxx
  编译器路径。这里直接使用 NDK 的 clang 包装脚本。

--enable-shared / --disable-static
  生成 .so 动态库，不生成 .a 静态库。
  Android app 通过 System.loadLibrary() 加载动态库。

--disable-programs
  不编译 ffmpeg / ffplay / ffprobe 命令行工具。
  Android 上不需要这些，我们直接在 C++ 里调用 FFmpeg API。

--disable-doc
  不生成文档 (节省构建时间)。

--disable-avdevice
  不编译设备采集模块。
  摄像头、麦克风采集是 Linux 推流端的活，Android 播放端不需要。

--disable-avfilter
  不编译滤镜模块。
  不用美颜，不用特效，不用转场。

--disable-postproc
  不编译后处理模块。

--disable-encoders
  不编译任何编码器。
  Android 播放端只需要解码，编码是 Linux 推流端的活。
  (如果未来做反向链路 Android 推流，需要加回来)

--disable-muxers
  不编译任何封装器 (FLV muxer, MP4 muxer...)。
  拉流端输出的是解码后的原始帧，不需要重新封装。

--disable-devices
  不编译设备输入输出 (v4l2, alsa, fbdev...)。

--disable-filters
  不编译任何滤镜。

--enable-swscale
  保留图像缩放和像素格式转换。
  ★ 必需: H.264 解码输出 YUV420P，渲染需要 RGBA。

--enable-swresample
  保留音频重采样和格式转换。
  ★ 必需: AAC 解码输出 FLTP，AAudio 需要 S16 交错格式。

--enable-protocol=rtmp
  RTMP 协议支持。这是拉流的核心。

--enable-protocol=file
  文件协议支持。用于播放本地测试文件。

--enable-demuxer=flv,live_flv
  FLV 解封装器。RTMP 流的内容是 FLV 格式封装的。

--enable-decoder=h264,aac
  H.264 视频解码器和 AAC 音频解码器。
  这是 FFmpeg 的内置解码器 (native decoder)，不需要外部依赖。

--enable-parser=h264,aac
  H.264 和 AAC 的码流解析器。
  解码器需要 parser 来从原始码流中识别 NAL unit 和帧边界。

--disable-asm
  ★ 禁用汇编优化。在 Windows 上交叉编译 Android 时:
  - 汇编器可能找不到或版本不匹配
  - 汇编语法可能不被识别
  - 禁用后性能略有下降 (10-20%)，但在开发阶段完全可接受
  - 如果一切顺利可以去掉这个选项，启用 NEON 汇编优化

--extra-cflags="-O2 -fPIC -DANDROID"
  -O2:      优化级别 2 (速度和体积的平衡)
  -fPIC:    生成位置无关代码 (动态库必需)
  -DANDROID: 定义 ANDROID 宏，让代码中的 #ifdef __ANDROID__ 生效
```

### 5.3 运行构建

```bash
# Windows Git Bash
bash scripts/build-ffmpeg-android.sh
```

整个构建过程：
```text
1. 下载 FFmpeg 源码 (~10s 取决于网络)
2. 解压 (~5s)
3. ./configure (~30s)
4. make (~5-15 分钟，取决于 CPU)
5. make install (~10s)
```

### 5.4 configure 的典型输出

成功执行 `./configure` 后，你会看到类似这样的摘要：

```text
install prefix            /d/code/StreamBridge/third_party/ffmpeg-android/arm64-v8a
source path               .
C compiler                aarch64-linux-android21-clang
C library                 bionic
host C compiler           gcc
ARCH                      aarch64 (armv8-a)
big-endian                no
runtime cpu detection     yes
standalone assembly       yes
aarch64 assembly          no          ← --disable-asm 的效果
MMX enabled               no
MMXEXT enabled            no
...
Enabled decoders:
aac                     h264
Enabled demuxers:
flv                     live_flv
Enabled protocols:
file                    rtmp
...
```

---

## 6. 构建产物

### 6.1 make install 后的目录结构

```text
third_party/ffmpeg-android/arm64-v8a/
├── include/                        # ★ C/C++ 头文件
│   ├── libavformat/
│   │   ├── avformat.h              # 封装/解封装 API
│   │   └── avio.h                  # I/O 上下文
│   ├── libavcodec/
│   │   ├── avcodec.h               # 编解码 API
│   │   ├── codec.h                 # 编解码器描述
│   │   └── codec_id.h              # 编解码器 ID 枚举
│   ├── libavutil/
│   │   ├── avutil.h                # 工具函数
│   │   ├── frame.h                 # AVFrame
│   │   ├── rational.h              # AVRational
│   │   ├── error.h                 # 错误码
│   │   ├── log.h                   # 日志
│   │   ├── pixfmt.h                # 像素格式
│   │   ├── samplefmt.h             # 采样格式
│   │   ├── channel_layout.h        # 声道布局
│   │   └── mem.h                   # 内存管理
│   ├── libswscale/
│   │   └── swscale.h               # 图像缩放/转换
│   └── libswresample/
│       └── swresample.h            # 音频重采样
├── lib/                            # ★ 动态库文件
│   ├── libavformat.so              # 封装/解封装 (~2-3 MB)
│   ├── libavcodec.so               # 编解码器 (~5-8 MB)
│   ├── libavutil.so                # 工具函数 (~500 KB)
│   ├── libswscale.so               # 图像转换 (~400 KB)
│   └── libswresample.so            # 音频转换 (~100 KB)
└── share/
    └── ffmpeg/
        └── examples/               # 示例代码 (可以忽略)
```

### 6.2 动态库依赖关系

```text
libavformat.so
  ├── 依赖 libavcodec.so
  ├── 依赖 libavutil.so
  └── 依赖 libswresample.so

libavcodec.so
  ├── 依赖 libavutil.so
  └── 依赖 libswresample.so

libswscale.so
  └── 依赖 libavutil.so

libswresample.so
  └── 依赖 libavutil.so

libavutil.so
  └── (无其它 FFmpeg 依赖)
```

**在 Android 上加载时的顺序**：

```java
// Java 中加载 .so 的顺序必须满足依赖关系！
System.loadLibrary("avutil");       // 最先: 没有 FFmpeg 依赖
System.loadLibrary("swresample");   // 第二: 只依赖 avutil
System.loadLibrary("swscale");      // 第三: 只依赖 avutil
System.loadLibrary("avcodec");      // 第四: 依赖 avutil + swresample
System.loadLibrary("avformat");     // 最后: 依赖上面所有
```

### 6.3 验证构建产物

```bash
# 1. 确认是 ARM64 ELF
$ file libavcodec.so
libavcodec.so: ELF 64-bit LSB shared object, ARM aarch64, ...

# 2. 确认没有未解析的符号
$ llvm-nm -u libavformat.so
# (应该只有 Bionic libc 的符号，如 printf, malloc 等)

# 3. 查看导出符号
$ llvm-nm -D libavcodec.so | grep avcodec_send_packet
00012345 T avcodec_send_packet  ← T = 已导出

# 4. 查看依赖
$ llvm-readelf -d libavformat.so | grep NEEDED
0x... (NEEDED)  Shared library: [libavcodec.so.62]
0x... (NEEDED)  Shared library: [libavutil.so.60]
0x... (NEEDED)  Shared library: [libc.so]
```

---

## 7. 集成到 Android 项目

### 7.1 复制 .so 文件

```bash
# 创建 Android 原生库目录
mkdir -p android/native/libs/arm64-v8a/

# 复制 FFmpeg .so 文件
cp third_party/ffmpeg-android/arm64-v8a/lib/*.so \
   android/native/libs/arm64-v8a/
```

### 7.2 配置 Android.mk (ndk-build)

```makefile
# android/native/Android.mk

LOCAL_PATH := $(call my-dir)

# ========== 1. 声明预编译的 FFmpeg 库 ==========

# libavutil
include $(CLEAR_VARS)
LOCAL_MODULE := avutil
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libavutil.so
include $(PREBUILT_SHARED_LIBRARY)

# libswresample
include $(CLEAR_VARS)
LOCAL_MODULE := swresample
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libswresample.so
include $(PREBUILT_SHARED_LIBRARY)

# libswscale
include $(CLEAR_VARS)
LOCAL_MODULE := swscale
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libswscale.so
include $(PREBUILT_SHARED_LIBRARY)

# libavcodec
include $(CLEAR_VARS)
LOCAL_MODULE := avcodec
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libavcodec.so
include $(PREBUILT_SHARED_LIBRARY)

# libavformat
include $(CLEAR_VARS)
LOCAL_MODULE := avformat
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libavformat.so
include $(PREBUILT_SHARED_LIBRARY)

# ========== 2. 编译我们的库 ==========

include $(CLEAR_VARS)
LOCAL_MODULE := streambridge_android

LOCAL_SRC_FILES := \
    jni/streambridge_jni.cpp \
    playback/native_audio_output.cpp \
    playback/playback_clock.cpp \
    playback/native_playback_session.cpp \
    playback/native_video_renderer.cpp \
    playback/ffmpeg/ffmpeg_subscriber.cpp \
    playback/ffmpeg/ffmpeg_video_decoder.cpp \
    playback/ffmpeg/ffmpeg_audio_decoder.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/playback \
    $(LOCAL_PATH)/../../common/include \
    $(LOCAL_PATH)/../../third_party/ffmpeg-android/arm64-v8a/include

LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra
LOCAL_LDLIBS := -laaudio -landroid -llog

# ★ 链接 FFmpeg 预编译库
LOCAL_SHARED_LIBRARIES := avformat avcodec swscale swresample avutil

include $(BUILD_SHARED_LIBRARY)
```

### 7.3 配置 CMakeLists.txt (备用)

如果你用 CMake 而不是 ndk-build：

```cmake
cmake_minimum_required(VERSION 3.22.1)
project(streambridge_android LANGUAGES CXX)

# FFmpeg 导入
set(FFMPEG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg-android/arm64-v8a)

add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libavutil.so)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libswresample.so)

add_library(swscale SHARED IMPORTED)
set_target_properties(swscale PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libswscale.so)

add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libavcodec.so)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libavformat.so)

# 我们的库
add_library(streambridge_android SHARED
    jni/streambridge_jni.cpp
    playback/native_audio_output.cpp
    playback/playback_clock.cpp
    playback/native_playback_session.cpp
    playback/native_video_renderer.cpp
    # ... ffmpeg 文件
)

target_compile_features(streambridge_android PRIVATE cxx_std_17)

target_include_directories(streambridge_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/playback
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${FFMPEG_ROOT}/include
)

target_link_libraries(streambridge_android
    avformat avcodec swscale swresample avutil
    aaudio android log
)
```

### 7.4 复制 .so 到 APK

`build.gradle` 中需要确保 FFmpeg .so 被复制到 APK 中。如果使用 `jniLibs`：

```groovy
android {
    sourceSets {
        main {
            jniLibs.srcDirs = ['../native/libs']
        }
    }
}
```

---

## 8. 常见问题与调试

### 8.1 configure: "C compiler cannot create executables"

```text
原因: 交叉编译器路径不对，或缺少必要的链接库
解决:
  1. 确认 NDK 路径正确: ls $NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android21-clang
  2. 检查 config.log 获取详细错误 (在 FFmpeg 源码目录)
  3. 尝试添加 --sysroot 显式指定
```

### 8.2 configure: "assembler not found" 或汇编错误

```text
原因: 交叉汇编器未正确配置
解决:
  1. 加 --disable-asm (最简单)
  2. 或者: export AS="llvm-as" 并确保它在 PATH 中
```

### 8.3 make: "undefined reference to ..."

```text
原因: 缺少某个系统库的链接
常见缺失:
  - libz   → 添加 --enable-zlib 或 --disable-zlib
  - libbz2 → 添加 --disable-bzlib
解决: 对于 Android，通常 --disable-zlib --disable-bzlib
```

### 8.4 .so 文件为什么这么大？

```text
libavcodec.so ~8MB (调试符号) → strip 后 ~5MB
完整 FFmpeg libavcodec 可能 20MB+

减小体积的方法:
  1. strip --strip-unneeded *.so          (去掉不必要的符号)
  2. --enable-small                       (FFmpeg configure 选项，牺牲速度换体积)
  3. 只启用需要的解码器 (我们已经这样做了)
  4. 使用 LTO: --extra-cflags="-flto"
```

### 8.5 Application.mk: APP_STL 不兼容

```text
NDK r28 使用 libc++_shared.so。
FFmpeg 默认使用 Bionic libc (没有 C++ 部分)。
如果链接时报 std::* 符号找不到:
  确保 APP_STL := c++_shared (在 Application.mk 或 build.gradle 中)
```

### 8.6 API level 选择

```text
API 21 (Android 5.0): 覆盖 99%+ 设备，功能足够
API 24 (Android 7.0): 增加了一些 NDK 多媒体 API
API 26 (Android 8.0): AAudio 首次引入

建议: 编译用 API 21，运行时不使用低于 API 26 的 AAudio 新功能
```

---

## 9. 总结

### 9.1 核心要点回顾

1. **交叉编译 = 在 Host 上编译 Target 平台的代码**
   - Host: x86_64 Windows
   - Target: aarch64 Android (ARM64)

2. **工具链是关键**
   - NDK 提供了完整的交叉编译工具链
   - `aarch64-linux-android21-clang` 一站式解决编译问题

3. **FFmpeg 的 configure 需要明确告诉它一切**
   - `--enable-cross-compile` 是最重要的开关
   - `--target-os=android` 告诉 FFmpeg 目标平台
   - 精简编译可以减少 80%+ 的体积

4. **构建产物是标准的 ELF .so 文件**
   - 可以像使用普通库一样集成到 Android 项目中
   - 加载顺序由依赖关系决定

### 9.2 给初学者的建议

如果你第一次做交叉编译，建议按这个顺序学习：

```text
1. 先用 NDK 编译一个 hello.c，理解交叉编译器如何工作
   $ aarch64-linux-android21-clang hello.c -o hello
   $ file hello  # 看看输出是什么

2. 编译一个小的 C 库 (比如 zlib)，理解 configure/make 交叉编译模式
   $ CC=aarch64-linux-android21-clang ./configure --prefix=...
   $ make && make install

3. 再挑战 FFmpeg 这种大型项目
   - 先用默认配置编译成功
   - 再逐步精简
```

### 9.3 本项目的构建指令

```bash
# 一键构建 FFmpeg for Android
bash scripts/build-ffmpeg-android.sh

# 构建完成后:
# 1. 复制 .so 到 android/native/libs/arm64-v8a/
# 2. 在 Android.mk 中配置预编译库
# 3. gradlew :android:app:assembleDebug
```

---

## 参考资料

- [Android NDK 官方文档](https://developer.android.com/ndk/guides)
- [FFmpeg 编译指南](https://trac.ffmpeg.org/wiki/CompilationGuide)
- [FFmpeg 交叉编译文档](https://trac.ffmpeg.org/wiki/CompilationGuide/CrossCompiling)
- [GNU Build System (autotools) 交叉编译](https://www.gnu.org/software/automake/manual/html_node/Cross_002dCompilation.html)
