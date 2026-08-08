# V4L2 (Video4Linux2) 深度讲解

> 适用场景：Linux 视频采集、摄像头驱动开发、嵌入式视频应用、音视频工程面试准备。
> 本文侧重原理、API 语义、数据流和面试高频考点，不依赖特定硬件。

---

## 0. 面试高频问题

以下问题覆盖 V4L2 面试中最常被问到的知识点，建议在阅读正文前先自测一遍。

### 基础概念

| # | 问题 | 考察点 |
|---|------|--------|
| 1 | V4L2 是什么？它在 Linux 内核中处于哪个层次？ | V4L2 定位、内核子系统关系 |
| 2 | V4L2 的编程模型是怎样的？核心操作流程是什么？ | ioctl 模型、open → format → buffer → stream |
| 3 | V4L2 支持哪三种 buffer I/O 模式？各自的优缺点和使用场景是什么？ | MMAP、USERPTR、DMABUF |
| 4 | `VIDIOC_REQBUFS`、`VIDIOC_QUERYBUF`、`VIDIOC_QBUF`、`VIDIOC_DQBUF` 分别做什么？调用顺序是什么？ | buffer 生命周期 |
| 5 | V4L2 中 `v4l2_format` 的 `type` 字段有哪些常见值？`V4L2_BUF_TYPE_VIDEO_CAPTURE` vs `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` 有什么区别？ | 格式类型、多平面 |
| 6 | 什么是 V4L2 control？如何枚举、读取和设置 control？ | controls 机制 |
| 7 | `select()`/`poll()` 在 V4L2 中的作用是什么？为什么需要它们？ | 异步 I/O、阻塞 vs 非阻塞 |
| 8 | V4L2 的像素格式（pixel format / FOURCC）是什么？常见的有哪些？如何查询设备支持的格式？ | FOURCC、格式协商 |

### 进阶与工程实践

| # | 问题 | 考察点 |
|---|------|--------|
| 9 | 解释 V4L2 buffer 的 `flags` 字段，`V4L2_BUF_FLAG_ERROR`、`V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` 各自意味着什么？ | buffer 状态、时间戳 |
| 10 | V4L2 时间戳的来源和类型有哪些？`timestamp` 和 `timestamp_type` 如何配合使用？与 `CLOCK_MONOTONIC` 的关系是什么？ | 时间戳、时钟源 |
| 11 | 什么是 V4L2 的 "streaming" 模式 vs "read/write" 模式？为什么实际项目中几乎只用 streaming？ | 性能、零拷贝 |
| 12 | 如果摄像头拔掉或发生 USB 错误，V4L2 应用应该如何检测和处理？ | 错误处理、热插拔 |
| 13 | 如何用 `v4l2-ctl` 命令行工具枚举设备、查看能力、设置格式？列举 5 个常用命令。 | 调试工具 |
| 14 | V4L2 的 crop、selection、scaling 是什么关系？`VIDIOC_S_CROP` 和 `VIDIOC_S_SELECTION` 有什么区别？ | 图像裁剪与缩放 |
| 15 | DMABUF I/O 模式相比 MMAP 的优势在哪里？什么场景下必须使用 DMABUF？ | 零拷贝、跨设备共享 |
| 16 | V4L2 在 `VIDIOC_STREAMON` 之前和 `VIDIOC_STREAMOFF` 之后分别应该做什么？掉步骤会有什么后果？ | 流生命周期 |
| 17 | 多平面（multi-planar）格式的必要性是什么？哪些编码格式会用到它？ | NV12、YUV 分平面存储 |
| 18 | 如果要在同一进程中同时采集两个摄像头，需要注意什么？ | 多设备管理、线程模型 |
| 19 | FFmpeg 是如何封装 V4L2 的？`avdevice` 中 V4L2 的输入参数有哪些关键选项？ | FFmpeg 集成 |
| 20 | 采集到的帧率低于预期，可能的原因有哪些？如何排查？ | 性能调优、调度延迟 |

### 答案线索

下面给出每个问题的核心要点（详细原理见正文各章节）：

1. **V4L2 是什么** — Video4Linux version 2，Linux 内核的视频设备驱动框架，提供用户空间统一 API 来访问摄像头、TV 卡、视频采集卡等。处于内核 video4linux 子系统中，对上暴露 `/dev/videoN` 字符设备，对下通过 `v4l2_subdev`、`media_controller` 与硬件驱动对接。

2. **编程模型** — 所有操作通过 `ioctl()` 完成。标准流程：`open()` → `VIDIOC_QUERYCAP` → `VIDIOC_S_FMT` → `VIDIOC_REQBUFS` → `VIDIOC_QUERYBUF` (mmap) → `VIDIOC_QBUF` → `VIDIOC_STREAMON` → 循环 `VIDIOC_DQBUF`/处理/`VIDIOC_QBUF` → `VIDIOC_STREAMOFF` → 释放 buffer → `close()`。

3. **三种 I/O 模式** —
   - **MMAP**（最常用）：内核分配 buffer，应用层 `mmap()` 映射到用户空间，零拷贝（内核直接 DMA 到共享 buffer），应用无需关心内存分配。
   - **USERPTR**：应用层自行分配内存，内核将数据 DMA 到用户提供的地址。灵活但需要处理页面对齐和物理连续性。
   - **DMABUF**：跨设备零拷贝，buffer 在设备间通过 dma-buf fd 共享（例如 V4L2 采集 → DRM/GPU 渲染），无需 CPU 拷贝。

4. **核心 ioctl 顺序** —
   - `VIDIOC_REQBUFS`：请求内核分配 N 个 buffer，指定 I/O 模式（MMAP/USERPTR/DMABUF）。
   - `VIDIOC_QUERYBUF`：查询每个 buffer 的物理偏移/用户指针/fd 和长度，供 mmap 使用。
   - `VIDIOC_QBUF`：将空 buffer 入队，交给内核填充数据。
   - `VIDIOC_DQBUF`：取出已被内核填充数据的 buffer，处理完毕后重新 QBUF。

5. **格式类型** —
   - `V4L2_BUF_TYPE_VIDEO_CAPTURE`：单平面（contiguous），Y/U/V 交错或打包在一个 buffer 中。
   - `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`：多平面，Y 和 UV 分量分开放到独立的内存平面。NV12、NV21 等半平面格式和需要平面分离的编码器常用。

6. **Controls** — V4L2 的通用属性设置接口，亮度、对比度、曝光、白平衡等都通过 control 操作。`VIDIOC_QUERYCTRL` 查询单个 control、`VIDIOC_QUERY_EXT_CTRLS` 批量查询、`VIDIOC_G_CTRL`/`VIDIOC_S_CTRL` 读写、`VIDIOC_G_EXT_CTRLS`/`VIDIOC_S_EXT_CTRLS` 批量读写。还可订阅 control 变化事件。

7. **poll 机制** — V4L2 设备可以非阻塞打开（`O_NONBLOCK`），然后用 `select()`/`poll()` 等待数据就绪。不这样做的话，`VIDIOC_DQBUF` 在无数据时会阻塞。poll 允许单线程管理多个设备，或与 UI 事件循环集成。

8. **FOURCC** — 四字符码（Four Character Code），标识像素格式。常见：`YUYV`(YUYV 4:2:2)、`UYVY`、`NV12`、`NV21`、`YUV420`、`MJPEG`、`H264`。通过 `VIDIOC_ENUM_FMT` 枚举设备支持的全部格式。

9. **buffer flags** —
   - `V4L2_BUF_FLAG_ERROR`：buffer 数据可能损坏（USB 传输错误等），应用应丢弃此帧。
   - `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`：时间戳基于 `CLOCK_MONOTONIC`（不受系统时间跳变影响）。
   - `V4L2_BUF_FLAG_KEYFRAME`/`PFRAME`/`BFRAME`：压缩流的帧类型。
   - `V4L2_BUF_FLAG_TIMECODE`：buffer 包含 VITC 或 LTC timecode。

10. **时间戳** — `v4l2_buffer.timestamp` 是 `struct timeval`，`timestamp_type` 有：`UNKNOWN`、`MONOTONIC`（CLOCK_MONOTONIC，推荐）、`COPY_OF_SOURCE`。重点：采集到的时间戳必须转换为统一时间基（如微秒）供下游编码和同步使用。

11. **Streaming vs read/write** — `read()`/`write()` 每次调用都涉及一次内核-用户态拷贝，适合低速简单场景。Streaming（MMAP）在内核 DMA 完成后应用直接读共享内存，零拷贝，是高性能场景的唯一选择。

12. **热插拔/错误** — `VIDIOC_DQBUF` 返回 `-ENODEV` 表示设备已断开；`-EIO` 表示 I/O 错误。应用应实现重试/重连逻辑（包括 `close()` 重新 `open()`），并区分临时错误和永久错误。

13. **v4l2-ctl 常用命令** —
    ```bash
    v4l2-ctl --list-devices                  # 列出所有设备
    v4l2-ctl -d /dev/video0 --all            # 显示全部信息
    v4l2-ctl -d /dev/video0 --list-formats-ext # 列出所有格式及分辨率/帧率
    v4l2-ctl -d /dev/video0 --set-fmt-video=width=1280,height=720,pixelformat=YUYV
    v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10  # 采集 10 帧
    ```

14. **Crop/Selection/Scaling** —
    - Crop（`VIDIOC_S_CROP`）：从传感器全尺寸中选择一个矩形区域，老 API。
    - Selection（`VIDIOC_S_SELECTION`）：更通用的矩形选择，支持 compose/crop/bounds 等多种 target。
    - Scaling：将 crop 后的图像缩放到 `S_FMT` 指定的输出尺寸，由硬件 scaler 完成。

15. **DMABUF 优势** — 跨设备零拷贝。V4L2 采集的 buffer 通过 dma-buf fd 直接传给 DRM/GPU 做渲染，或传给硬件编码器，全程不需要 CPU 拷贝。Android Camera HAL2/3 和 GStreamer 重度依赖此模式。

16. **STREAMON 前后** —
    - 前：format 已设置，buffer 已申请并全部 QBUF。
    - 后：先 STREAMOFF，再 free mmap 映射，最后 `VIDIOC_REQBUFS(count=0)` 释放 kernel buffer。在 STREAMOFF 后可能有残留在队列中的 buffer，应在 free 前全部 DQBUF 出来。

17. **多平面（MPLANE）** — 对于 NV12 这类 Y 和 UV 分量大小不同的格式，单平面模式需要手动计算偏移。MPLANE 将每个分量独立存放在 `v4l2_plane` 数组中，硬件和驱动负责正确的偏移和步长，解码器输入/输出通常必须用 MPLANE。

18. **多摄像头** — 每个设备独立 `open()` 获得 fd，独立的 format 和 buffer 队列。通常每个设备一个采集线程。注意 USB 带宽共享问题（同一 USB 控制器下的多个摄像头可能互相争抢带宽）。

19. **FFmpeg 封装** — `avdevice` 的 `v4l2` 输入：
    ```bash
    ffmpeg -f v4l2 -input_format yuyv422 -video_size 1280x720 -framerate 30 -i /dev/video0 ...
    ```
    关键选项：`-input_format`、`-video_size`、`-framerate`、`-ts`（时间戳类型）。FFmpeg 内部使用 MMAP 模式，每帧回调给 `AVFormatContext`。

20. **帧率偏低排查** — 检查曝光时间（低光下自动曝光会拉低帧率）→ `v4l2-ctl -L` 查看 auto_exposure → 实际用 `v4l2-ctl --stream-mmap --stream-count=100` 测裸性能 → 检查 USB 带宽（`lsusb -t`）→ 检查调度延迟（`cyclictest`）→ 检查应用层处理耗时。

---

## 1. V4L2 概述

### 1.1 什么是 V4L2

**Video4Linux2 (V4L2)** 是 Linux 内核为视频采集设备提供的标准驱动框架和用户空间 API。它是 V4L 的第二代版本，从 Linux 2.6 内核开始引入，至今仍然是 Linux 视频设备编程的核心基础设施。

在 Linux 内核栈中，V4L2 的位置：

```text
┌──────────────────────────────────────┐
│         User-space Application       │
│    (ffmpeg, gstreamer, opencv, ...)  │
├──────────────────────────────────────┤
│         System Call Interface        │
│    open/close/ioctl/mmap/select      │
├──────────┬───────────────────────────┤
│  /dev/   │  V4L2 Core (v4l2-common)  │
│ videoN   │  - format negotiation     │
│          │  - buffer management       │
│          │  - control framework       │
│          │  - event subscription      │
├──────────┼───────────────────────────┤
│  v4l2_   │  V4L2 Subdev (v4l2-subdev)│
│ subdevN  │  - sensor/lens/flash       │
│          │  - bridge chip control     │
├──────────┼───────────────────────────┤
│ mediaN   │  Media Controller          │
│          │  - topology discovery      │
│          │  - pad/link management     │
├──────────┴───────────────────────────┤
│         Hardware Drivers             │
│  uvcvideo / vivid / bcm2835-unicam   │
│  ipu3-cio2 / intel-ipu6 / ...       │
├──────────────────────────────────────┤
│         Hardware (USB / PCIe / MIPI) │
└──────────────────────────────────────┘
```

### 1.2 核心设计理念

V4L2 的设计贯穿几条关键思想：

1. **一切皆 ioctl** — 所有配置、查询、控制操作都通过 `ioctl()` 完成，数据通过 `mmap()` + 队列传递。这是为了在保持系统调用语义清晰的同时，允许驱动实现复杂的状态机。

2. **能力协商而非假设** — 应用必须先查询设备能力（`VIDIOC_QUERYCAP`），再枚举支持的格式/分辨率/帧率（`VIDIOC_ENUM_FMT` / `VIDIOC_ENUM_FRAMESIZES` / `VIDIOC_ENUM_FRAMEINTERVALS`），最后才设置期望的参数。硬编码 `/dev/video0`、1280x720、YUYV、30fps 会在不同设备上失败。

3. **零拷贝优先** — 通过 MMAP 或 DMABUF 模式让硬件 DMA 直接写入与用户空间共享的内存，避免 `read()` 的额外拷贝。

4. **驱动抽象** — 上层代码（FFmpeg、GStreamer、应用）使用同一套 API 操作不同硬件（USB UVC 摄像头、CSI/MIPI 传感器、PCIe 采集卡），差异由内核驱动层屏蔽。

### 1.3 设备节点

V4L2 在 `/dev/` 下创建三类设备节点：

| 节点 | 用途 | 示例 |
|------|------|------|
| `/dev/videoN` | 视频流数据通道（主设备） | `/dev/video0` |
| `/dev/v4l-subdevN` | 子设备控制（sensor/flash/chip） | `/dev/v4l-subdev0` |
| `/dev/mediaN` | Media Controller 拓扑信息 | `/dev/media0` |

应用通常只需要操作 `/dev/videoN`。子设备和 media 节点用于更底层的 ISP/传感器 pipeline 控制。

---

## 2. 核心数据结构

### 2.1 `v4l2_capability` — 设备能力

```c
struct v4l2_capability {
    __u8  driver[16];      // 驱动名称，如 "uvcvideo"
    __u8  card[32];        // 设备名称，如 "HD Webcam C920"
    __u8  bus_info[32];    // 总线信息，如 "usb-0000:00:14.0-1"
    __u32 version;         // 驱动版本号（KERNEL_VERSION 宏格式）
    __u32 capabilities;    // 设备能力位掩码
    __u32 device_caps;     // 当前节点能力（V4L2 多节点设备中比 capabilities 更精确）
    __u32 reserved[3];
};
```

关键 `device_caps` 位：

| 宏 | 含义 |
|----|------|
| `V4L2_CAP_VIDEO_CAPTURE` | 支持视频采集 |
| `V4L2_CAP_VIDEO_CAPTURE_MPLANE` | 支持多平面视频采集 |
| `V4L2_CAP_VIDEO_OUTPUT` | 支持视频输出 |
| `V4L2_CAP_VIDEO_M2M` | Memory-to-Memory 设备（编解码器/缩放器） |
| `V4L2_CAP_STREAMING` | 支持 Streaming I/O（MMAP/USERPTR） |
| `V4L2_CAP_READWRITE` | 支持 read()/write() |
| `V4L2_CAP_TIMEPERFRAME` | 支持逐帧时间间隔配置 |

**面试理解要点**：`capabilities` 是整个物理设备的能力，`device_caps` 是当前 `/dev/videoN` 节点的能力。现代 UVC 设备一个物理摄像头可能对应两个 `/dev/videoN` 节点（video0 为 metadata，video1 为实际视频流），此时 `device_caps` 区分二者。

### 2.2 `v4l2_format` — 数据格式

```c
struct v4l2_format {
    __u32 type;                       // 流类型，如 V4L2_BUF_TYPE_VIDEO_CAPTURE
    union {
        struct v4l2_pix_format        pix;      // 单平面视频
        struct v4l2_pix_format_mplane pix_mp;   // 多平面视频
        struct v4l2_window            win;      // overlay
        struct v4l2_vbi_format        vbi;      // VBI
        struct v4l2_sliced_vbi_format sliced;
        struct v4l2_sdr_format        sdr;      // Software Defined Radio
        struct v4l2_meta_format       meta;     // metadata
        __u8                         raw_data[200];
    } fmt;
};
```

#### 单平面格式 `v4l2_pix_format`

```c
struct v4l2_pix_format {
    __u32 width;         // 图像宽度（像素）
    __u32 height;        // 图像高度（像素）
    __u32 pixelformat;   // FOURCC 像素格式
    __u32 field;         // 场序（逐行=ANY/NONE，交错=TOP/BOTTOM/INTERLACED）
    __u32 bytesperline;  // 每行字节数（含 padding，通常 > width * bpp/8）
    __u32 sizeimage;     // 整帧缓冲区大小（bytes）
    __u32 colorspace;    // 色彩空间（sRGB / Rec.709 / ...）
    __u32 priv;          // 私有数据
    __u32 flags;         // 格式标志
    union {
        __u32 ycbcr_enc; // Y'CbCr 编码
        __u32 hsv_enc;   // HSV 编码
    };
    __u32 quantization;  // 量化范围（full range / limited range）
    __u32 xfer_func;     // 传输函数（sRGB / Rec.709 / ...）
};
```

**关键语义**：
- `bytesperline` ≥ `width * bytes_per_pixel`，多出的部分为 stride padding。硬件可能有对齐要求（如 64 或 128 字节对齐），不要假设等于 `width * bpp/8`。
- `sizeimage` 是驱动估算的最大帧大小，应用按此分配 buffer。不要自行计算（format 协商后 `sizeimage` 可能因编解码器的码率控制而大于 `width * height * 帧/打包比例`）。
- `field` 在现代摄像头中通常是 `V4L2_FIELD_NONE`（逐行扫描），但交错设备（模拟采集）可能是 `V4L2_FIELD_INTERLACED`。
- `colorspace` / `ycbcr_enc` / `quantization` / `xfer_func` 这组字段在 V4L2 2.6.36+ 中引入，用于描述准确的色彩语义。UVC 设备通常正确填充，但 V4L2 虚拟设备（vivid）和某些老驱动可能不填。

#### 多平面格式 `v4l2_pix_format_mplane`

```c
struct v4l2_pix_format_mplane {
    __u32 width, height;
    __u32 pixelformat, field;
    __u32 colorspace;
    struct v4l2_plane_pix_format {
        __u32 sizeimage;     // 该平面大小
        __u32 bytesperline;  // 该平面每行字节数
        __u16 reserved[6];
    } plane_fmt[VIDEO_MAX_PLANES];  // 最多 8 个平面
    __u8  num_planes;
    __u8  flags;
    union { __u32 ycbcr_enc; __u32 hsv_enc; };
    __u32 quantization, xfer_func;
    __u8  reserved[7];
};
```

多平面使用场景：
- NV12/NV21：2 个平面（Y 全分辨率 + UV 交错 1/2 分辨率）
- YUV420p 类：3 个平面（Y + U + V）
- 某些硬件编码器的输入格式

### 2.3 `v4l2_buffer` — 缓冲区描述符

```c
struct v4l2_buffer {
    __u32  index;               // buffer 序号
    __u32  type;                // stream 类型
    __u32  bytesused;           // 有效数据字节数（≤ length）
    __u32  flags;               // 状态标志
    __u32  field;               // 场序
    struct timeval timestamp;   // 时间戳
    struct v4l2_timecode timecode;
    __u32  sequence;            // 帧序号（从 STREAMON 起连续递增）
    __u32  memory;              // I/O 模式
    union {
        __u32  offset;          // MMAP: mmap 偏移
        unsigned long userptr;  // USERPTR: 用户空间地址
        __s32  fd;              // DMABUF: dma-buf fd
    } m;
    __u32  length;              // buffer 总长度
    __u32  reserved2;
    union {
        __u32  request_fd;      // request API
        /* 更多 reserved */
    };
};
```

#### 关键 `flags`

| 标志 | 含义 |
|------|------|
| `V4L2_BUF_FLAG_MAPPED` | 应用已 mmap（驱动设置） |
| `V4L2_BUF_FLAG_QUEUED` | buffer 在驱动队列中 |
| `V4L2_BUF_FLAG_DONE` | buffer 数据已就绪 |
| `V4L2_BUF_FLAG_ERROR` | 帧数据可能有误（USB 传输错误等），应丢弃 |
| `V4L2_BUF_FLAG_KEYFRAME` | 如果是压缩流，此帧为关键帧 |
| `V4L2_BUF_FLAG_PFRAME` | P 帧 |
| `V4L2_BUF_FLAG_BFRAME` | B 帧 |
| `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` | timestamp 使用 CLOCK_MONOTONIC |
| `V4L2_BUF_FLAG_TIMESTAMP_COPY` | timestamp 从源时间戳复制而来 |
| `V4L2_BUF_FLAG_LAST` | 流结束标记（decode 或 capture stop 时发送） |

#### 时间戳字段

```c
struct timeval {
    __kernel_time_t tv_sec;   // 秒
    __kernel_suseconds_t tv_usec; // 微秒（0-999999）
};
```

`timestamp` 的时钟源由 `flags` 中的 `V4L2_BUF_FLAG_TIMESTAMP_*` 决定。还可用 `VIDIOC_QUERYBUF` 通过 `timestamp_type` 查询：

```c
enum v4l2_buf_timestamp_type {
    V4L2_BUF_TIMESTAMP_TYPE_UNKNOWN        = 0,
    V4L2_BUF_TIMESTAMP_TYPE_MONOTONIC      = 1, // CLOCK_MONOTONIC（推荐）
    V4L2_BUF_TIMESTAMP_TYPE_COPY           = 2, // 复制自某个源时间戳
};
```

**StreamBridge 中**：取到 `CLOCK_MONOTONIC` 时间戳后，转换为微秒统一时间基：

```cpp
// timestamp 是 struct timeval
int64_t pts_us = static_cast<int64_t>(timestamp.tv_sec) * 1'000'000LL
               + static_cast<int64_t>(timestamp.tv_usec);
// 后续做首帧归一化：first_pts_us = pts_us; normalized_pts_us = pts_us - first_pts_us;
```

### 2.4 `v4l2_fmtdesc` / `v4l2_frmsizeenum` / `v4l2_frmivalenum` — 格式枚举

```c
struct v4l2_fmtdesc {
    __u32  index;                // 枚举序号（从 0 开始）
    __u32  type;                 // stream 类型
    __u32  flags;                // V4L2_FMT_FLAG_COMPRESSED 等
    __u8   description[32];      // 人类可读描述
    __u32  pixelformat;          // FOURCC
    __u32  mbus_code;            // media bus code (CSI 等)
    __u32  reserved[3];
};

struct v4l2_frmsizeenum {
    __u32  index;                // 枚举序号
    __u32  pixel_format;         // 要查询的 FOURCC
    __u32  type;                 // DISCRETE / CONTINUOUS / STEPWISE
    union {
        struct v4l2_frmsize_discrete discrete;
        struct v4l2_frmsize_stepwise stepwise;
    };
    __u32  reserved[2];
};

struct v4l2_frmivalenum {
    __u32  index;
    __u32  pixel_format;
    __u32  width, height;
    __u32  type;                 // DISCRETE / CONTINUOUS / STEPWISE
    union {
        struct v4l2_frmival_discrete discrete;
        struct v4l2_frmival_stepwise stepwise;
    };
    __u32  reserved[2];
};
```

枚举范式：

```text
VIDIOC_ENUM_FMT (index=0,1,2,...)
  └── 对每个 format:
        VIDIOC_ENUM_FRAMESIZES (index=0,1,2,...)
          └── 对每个 size:
                VIDIOC_ENUM_FRAMEINTERVALS (index=0,1,2,...)
```

**面试要点**：不同摄像头对同一个 FOURCC 可能支持离散的尺寸列表（type=DISCRETE），也可能支持连续或步进式尺寸（STEPWISE/CONTINUOUS）。应用不应该假设存在某种尺寸，而是先枚举再选择最接近目标的那个。

---

## 3. I/O 模式深度对比

### 3.1 总览

| 特性 | MMAP | USERPTR | DMABUF | read/write |
|------|------|---------|--------|------------|
| 内存分配方 | 内核 | 应用 | 内核(导出方) | 内核(每次) |
| 拷贝次数 | 0 | 0 | 0 | 1-2 |
| 复杂性 | 低 | 中 | 中 | 极低 |
| 跨设备共享 | 不支持 | 不支持 | **支持** | 不支持 |
| 内存控制 | 有限 | 完全 | 完全(导出方) | 无 |
| 典型场景 | 通用采集 | 自定义内存池 | 硬件编解码+渲染 | 低帧率快照 |
| `v4l2_buffer.m` 字段 | `offset` | `userptr` | `fd` | — |

### 3.2 MMAP（Memory Mapping）模式 — 最常用

```text
应用侧                                    内核侧
┌──────────┐       mmap()         ┌──────────────┐
│ 用户空间  │ ◄──────────────► │  内核缓冲区   │
│ 虚拟地址  │                     │  队列 (VIVT)  │
└──────────┘                     └──────┬───────┘
                                        │ DMA
                                   ┌────▼────────┐
                                   │  USB/CSI/PCIe │
                                   │    硬件       │
                                   └───────────────┘
```

完整流程：

```c
// Step 1: 请求分配 buffer
struct v4l2_requestbuffers req = {0};
req.count  = 4;                          // 至少 2，常见 4-8
req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_MMAP;
ioctl(fd, VIDIOC_REQBUFS, &req);         // 驱动可能返回 < count
// req.count = 驱动实际分配的数量

// Step 2: 查询每个 buffer 并 mmap
struct { void *start; size_t length; } buffers[req.count];

for (int i = 0; i < req.count; i++) {
    struct v4l2_buffer buf = {0};
    buf.type   = req.type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);

    buffers[i].length = buf.length;
    buffers[i].start  = mmap(NULL, buf.length,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, buf.m.offset);
    if (buffers[i].start == MAP_FAILED) {
        // 错误处理
    }
}

// Step 3: 将全部 buffer 入队
for (int i = 0; i < req.count; i++) {
    struct v4l2_buffer buf = {0};
    buf.type   = req.type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;
    ioctl(fd, VIDIOC_QBUF, &buf);
}

// Step 4: 开启流
int type = req.type;
ioctl(fd, VIDIOC_STREAMON, &type);

// Step 5: 采集循环
while (running) {
    struct v4l2_buffer buf = {0};
    buf.type   = req.type;
    buf.memory = V4L2_MEMORY_MMAP;

    // 阻塞等待数据（或 select/poll）
    ioctl(fd, VIDIOC_DQBUF, &buf);

    // 使用数据：buffers[buf.index].start 起始，buf.bytesused 有效长度
    process_frame(buffers[buf.index].start, buf.bytesused, buf.timestamp);

    // 重新入队
    ioctl(fd, VIDIOC_QBUF, &buf);
}

// Step 6: 停止流
ioctl(fd, VIDIOC_STREAMOFF, &type);

// Step 7: 释放资源
for (int i = 0; i < req.count; i++)
    munmap(buffers[i].start, buffers[i].length);

// 释放 kernel buffer
req.count = 0;
ioctl(fd, VIDIOC_REQBUFS, &req);
```

### 3.3 USERPTR（User Pointer）模式

应用自行分配内存并告知内核。**注意**：USB 设备通常需要 DMA 可访问的物理连续内存，而 `malloc`/`new` 可能不满足。使用 `posix_memalign` 对齐到页大小是基本要求，但仍可能失败（需 `VIDIOC_TRY_FMT` / `VIDIOC_REQBUFS` 验证）。

```c
// 分配页对齐内存
void *buf;
posix_memalign(&buf, getpagesize(), sizeimage);

struct v4l2_requestbuffers req = {0};
req.count  = 4;
req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_USERPTR;
ioctl(fd, VIDIOC_REQBUFS, &req);

// QBUF 时填入用户指针
struct v4l2_buffer buf = {0};
buf.type     = req.type;
buf.memory   = V4L2_MEMORY_USERPTR;
buf.index    = i;
buf.m.userptr = (unsigned long)user_pointer;
buf.length   = sizeimage;
ioctl(fd, VIDIOC_QBUF, &buf);
```

适用场景：应用有自己的内存池（如 Google Chrome 的 VideoCapture 模块），需要精确控制内存布局和生命周期。

### 3.4 DMABUF（DMA Buffer Sharing）模式

跨设备的零拷贝共享，是现代 Linux 视频 pipeline 的核心。

```text
         导出方                      导入方
    (V4L2 采集或 DRM)           (V4L2 编码器或 GPU)
   ┌──────────────┐          ┌──────────────┐
   │  /dev/video0  │  dma-buf │  /dev/video1  │
   │  CAPTURE      │─── fd ──►│  M2M (编码器) │
   └──────────────┘          └──────────────┘
   硬件 DMA → buffer          读同一块物理内存
```

```c
// 导出方（采集设备）- 与 MMAP 类似，但不需要 mmap
struct v4l2_requestbuffers req = {0};
req.count  = 4;
req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_DMABUF;
ioctl(fd, VIDIOC_REQBUFS, &req);

// 获取 dma-buf fd
struct v4l2_exportbuffer expbuf = {0};
expbuf.type  = req.type;
expbuf.index = 0;
ioctl(fd, VIDIOC_EXPBUF, &expbuf);
// expbuf.fd = dma-buf fd

// 导入方（编码器/GPU）
struct v4l2_buffer buf = {0};
buf.type    = V4L2_BUF_TYPE_VIDEO_OUTPUT; // M2M 设备输出侧
buf.memory  = V4L2_MEMORY_DMABUF;
buf.m.fd    = expbuf.fd; // 直接传递 fd
ioctl(encoder_fd, VIDIOC_QBUF, &buf);
```

### 3.5 关于 `select()`/`poll()`/`epoll()`

V4L2 设备在 `O_NONBLOCK` 打开时支持 poll：

```c
fd_set fds;
FD_ZERO(&fds);
FD_SET(fd, &fds);

struct timeval tv = {.tv_sec = 2, .tv_usec = 0}; // 2 秒超时

int r = select(fd + 1, &fds, NULL, NULL, &tv);
if (r == -1)   { /* 信号中断等 */ }
if (r == 0)    { /* 超时 */ }
if (r > 0)     { /* 有数据，调用 VIDIOC_DQBUF */ }
```

也可用 `poll()`/`epoll()`，更推荐用于多设备或多事件场景。关键点：poll 指示有数据可读时，不一定只有一个 buffer 可用（可能有多个 buffer 已完成 DMA），但推荐一次只 DQBUF 一个，处理完再取下一个，维持背压。

---

## 4. Controls — 属性控制框架

### 4.1 Control 基本操作

V4L2 Controls 提供统一的属性配置接口。每个 control 有唯一 ID（`V4L2_CID_*`）、类型、最小/最大/默认/步进值。

**常用 Controls**：

| Control ID | 类型 | 用途 |
|------------|------|------|
| `V4L2_CID_BRIGHTNESS` | integer | 亮度 |
| `V4L2_CID_CONTRAST` | integer | 对比度 |
| `V4L2_CID_SATURATION` | integer | 饱和度 |
| `V4L2_CID_HUE` | integer | 色调 |
| `V4L2_CID_AUTO_WHITE_BALANCE` | boolean | 自动白平衡开关 |
| `V4L2_CID_WHITE_BALANCE_TEMPERATURE` | integer | 色温（K） |
| `V4L2_CID_EXPOSURE_AUTO` | menu | 自动曝光模式 |
| `V4L2_CID_EXPOSURE_ABSOLUTE` | integer | 绝对曝光时间 |
| `V4L2_CID_GAIN` | integer | 模拟增益 |
| `V4L2_CID_AUTOFOCUS` | boolean | 自动对焦 |
| `V4L2_CID_FOCUS_ABSOLUTE` | integer | 焦点位置 |
| `V4L2_CID_FOCUS_AUTO` | boolean | 连续自动对焦 |
| `V4L2_CID_POWER_LINE_FREQUENCY` | menu | 抗频闪（50Hz/60Hz） |
| `V4L2_CID_ZOOM_ABSOLUTE` | integer | 数字变焦 |

### 4.2 枚举、查询、读写

```c
// 1. 枚举所有 controls
struct v4l2_queryctrl qctrl = {0};
qctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL; // 从第一个开始

while (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
    printf("Control: %s (id=0x%x), min=%d, max=%d, default=%d\n",
           qctrl.name, qctrl.id, qctrl.minimum, qctrl.maximum,
           qctrl.default_value);
    qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
}

// 2. 读取 control
struct v4l2_control ctrl = {0};
ctrl.id = V4L2_CID_BRIGHTNESS;
ioctl(fd, VIDIOC_G_CTRL, &ctrl);
printf("Brightness = %d\n", ctrl.value);

// 3. 设置 control
ctrl.value = 128;
ioctl(fd, VIDIOC_S_CTRL, &ctrl);

// 4. 批量操作（推荐用于多个 controls 原子更新）
struct v4l2_ext_control ext_ctrls[2] = {
    { .id = V4L2_CID_BRIGHTNESS, .value = 128 },
    { .id = V4L2_CID_CONTRAST,   .value = 64  },
};
struct v4l2_ext_controls ctrls = {
    .ctrl_class = V4L2_CTRL_CLASS_USER,
    .count      = 2,
    .controls   = ext_ctrls,
};
ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
```

### 4.3 高级特性

**Menu 类型**（枚举选项）：
```c
struct v4l2_querymenu menu = {0};
menu.id = V4L2_CID_EXPOSURE_AUTO;
for (menu.index = 0; ; menu.index++) {
    if (ioctl(fd, VIDIOC_QUERYMENU, &menu) != 0) break;
    printf("  %d: %s\n", menu.index, menu.name);
}
// 典型输出：
// 0: Auto Mode
// 1: Manual Mode
// 2: Shutter Priority Mode  (不一定都有)
// 3: Aperture Priority Mode (不一定都有)
```

**订阅 Control 变更事件**：
```c
struct v4l2_event_subscription sub = {0};
sub.type = V4L2_EVENT_CTRL;
sub.id   = V4L2_CID_FOCUS_ABSOLUTE;
ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

// 后续用 select() + VIDIOC_DQEVENT 接收事件
struct v4l2_event ev;
ioctl(fd, VIDIOC_DQEVENT, &ev);
// ev.u.ctrl 中包含变更的 control ID 和新值
```

---

## 5. 像素格式 (FOURCC) 对照表

### 5.1 常见采集格式

| FOURCC | 格式 | 平面数 | 典型 BPP | 说明 |
|--------|------|--------|---------|------|
| `YUYV` | YUYV 4:2:2 | 1 | 16 | 最通用的 USB 摄像头格式，几乎每款 UVC 设备都支持 |
| `UYVY` | UYVY 4:2:2 | 1 | 16 | YUYV 的字节序变种 |
| `NV12` | Y + UV 交错 | 2 | 12 | Android 首选格式，H.264 编码器常用输入 |
| `NV21` | Y + VU 交错 | 2 | 12 | 与 NV12 仅 UV 顺序互换 |
| `YV12` | Y + V + U | 3 | 12 | YUV420 planar |
| `YU12`(I420) | Y + U + V | 3 | 12 | YUV420 planar，与 YV12 仅 U/V 顺序不同 |
| `MJPEG` | Motion JPEG | 1 | 可变 | 压缩格式，降低 USB 带宽但需要解码 |
| `H264` | H.264 压缩流 | 1 | 可变 | 部分 UVC 设备支持硬件编码输出 |
| `RGB3`(RGB24) | RGB 8:8:8 | 1 | 24 | 非标准，少用 |
| `BGR4`(BGR32) | BGRA 8:8:8:8 | 1 | 32 | 部分摄像头支持 |
| `GREY`(GRAY8) | 8-bit 灰度 | 1 | 8 | 简单格式 |
| `YU12`→`YV12` 区别 | — | — | — | YU12: Y(0), U(1), V(2); YV12: Y(0), V(1), U(2) |

### 5.2 内存布局示例

**YUYV 4:2:2**（每个像素两个字节，每两个像素共享 UV）：
```text
Pixel:  Y0 U0 Y1 V0  Y2 U2 Y3 V2  ...
Byte:   [0][1][2][3] [4][5][6][7] ...
Index:  Y: [0][2][4][6]...  U: [1][5]...  V: [3][7]...
```

**NV12**（Y 平面 + UV 交错平面）：
```text
Plane 0 (Y):  width × height 字节，逐行连续
Plane 1 (UV): (width/2) × (height/2) × 2 字节
              U0 V0 U1 V1 U2 V2 ...
```

在 V4L2 MPLANE 模式下，`buf.m.planes[0]` 指向 Y 平面，`buf.m.planes[1]` 指向 UV 平面。

---

## 6. 完整采集流程（状态机视角）

```text
                    ┌─────────┐
                    │  CLOSED │
                    └────┬────┘
                         │ open("/dev/video0")
                         ▼
                    ┌─────────┐
              ┌────►│  OPEN   │◄────────────┐
              │     └────┬────┘             │
              │          │ QUERYCAP         │
              │          │ ENUM_FMT/FRMSIZE │ 重新 open
              │          ▼                  │
              │     ┌─────────┐             │
              │     │ FORMAT  │             │
              │     │ SET     │             │
              │     └────┬────┘             │
              │          │ REQBUFS          │
              │          ▼                  │
              │     ┌─────────┐             │
              │     │ BUFFERS │             │
              │     │ READY   │             │
              │     └────┬────┘             │
              │          │ QBUF all         │
              │          ▼                  │
              │     ┌─────────┐             │
              │     │ QUEUED  │             │
              │     └────┬────┘             │
              │          │ STREAMON         │
              │          ▼                  │
              │     ┌─────────┐             │
              │     │STREAMING│             │
              │     └────┬────┘             │
              │          │                  │
              │    ┌─────▼──────┐           │
              │    │ DQBUF 等待 │           │
              │    │ 处理帧数据  │──────► 循环
              │    │ QBUF 归还  │           │
              │    └─────┬──────┘           │
              │          │                  │
              │          │ STREAMOFF        │
              │          ▼                  │
              │     ┌─────────┐             │
              │     │STOPPED  │             │
              │     └────┬────┘             │
              │          │ munmap           │
              │          │ REQBUFS(count=0) │
              │          ▼                  │
              │     ┌─────────┐             │
              └─────┤  OPEN   │(可复用 fomat → REQBUFS)
                    └────┬────┘
                         │ close()
                         ▼
                    ┌─────────┐
                    │  CLOSED │
                    └─────────┘
```

从 `FORMAT SET` 到 `BUFFERS READY` 这一步是单向的：一旦调用了 `VIDIOC_REQBUFS`，就不能再改 format（除非先 `REQBUFS(count=0)` 释放所有 buffer）。

---

## 7. Crop、Selection 与 Scaling

### 7.1 概念对比

| API | 用途 | 时期 |
|-----|------|------|
| `VIDIOC_CROPCAP` / `VIDIOC_S_CROP` / `VIDIOC_G_CROP` | 裁剪掉传感器边界外的像素（与硬件 scaler 配合） | 老 API |
| `VIDIOC_S_SELECTION` / `VIDIOC_G_SELECTION` | 更通用的矩形选择，支持多种 target | 新 API，推荐 |
| `VIDIOC_S_FMT` | 最终输出格式（宽度/高度包含缩放） | 始终使用 |

### 7.2 Selection Targets

```c
struct v4l2_selection sel = {0};
sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

// Target: 按 pipeline 顺序
// ┌────────────────────────────────────────┐
// │ V4L2_SEL_TGT_CROP_BOUNDS               │ ← 传感器能提供的最大矩形
// │  ┌────────────────────────────────┐    │
// │  │ V4L2_SEL_TGT_CROP              │    │ ← 从 sensor 中裁出（数字裁剪）
// │  │  ┌──────────────────────┐      │    │
// │  │  │ V4L2_SEL_TGT_COMPOSE │      │    │ ← 在输出画布上的位置（缩放后）
// │  │  │                      │      │    │
// │  │  └──────────────────────┘      │    │
// │  └────────────────────────────────┘    │
// └────────────────────────────────────────┘
//                    ↓
//          VIDIOC_S_FMT 实际输出帧尺寸

// 获取 crop bounds（传感器原生区域）
sel.target = V4L2_SEL_TGT_CROP_BOUNDS;
ioctl(fd, VIDIOC_G_SELECTION, &sel);
printf("Native: %dx%d\n", sel.r.width, sel.r.height);

// 设置 crop 区域
sel.target = V4L2_SEL_TGT_CROP;
sel.r.left   = 0;
sel.r.top    = 0;
sel.r.width  = 1280;
sel.r.height = 720;
ioctl(fd, VIDIOC_S_SELECTION, &sel);

// 再设置输出格式（驱动自动缩放）
struct v4l2_format fmt = {0};
fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
fmt.fmt.pix.width       = 640;  // 从 1280x720 缩放到 640x360
fmt.fmt.pix.height      = 360;
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
ioctl(fd, VIDIOC_S_FMT, &fmt);
```

---

## 8. 调试与排错

### 8.1 v4l2-ctl 常用命令（速查表）

```bash
# ── 设备发现 ──
v4l2-ctl --list-devices                          # 列出所有 V4L2 设备及路径
v4l2-ctl -d /dev/video0 --list-formats-ext       # ⭐ 最常用：列出所有格式+分辨率+帧率
v4l2-ctl -d /dev/video0 --all                    # 输出所有信息（格式/controls/crop/selection）

# ── 格式配置 ──
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1280,height=720,pixelformat=YUYV
v4l2-ctl -d /dev/video0 --get-fmt-video          # 读回实际生效的格式（驱动可能不同于设置值）
v4l2-ctl -d /dev/video0 --try-fmt-video=width=1920,height=1080,pixelformat=NV12  # 测试但不提交

# ── Controls ──
v4l2-ctl -d /dev/video0 --list-ctrls             # 列出所有 controls 和当前值
v4l2-ctl -d /dev/video0 --set-ctrl=brightness=128,contrast=64
v4l2-ctl -d /dev/video0 --get-ctrl=exposure_auto # 读取单个 control

# ── 采集测试 ──
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=100         # 采集 100 帧并丢帧率报告
v4l2-ctl -d /dev/video0 --stream-mmap --stream-to=test.raw       # 采集并保存到文件
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=100 --verbose  # verbose 输出每帧信息

# ── 高级 ──
v4l2-ctl -d /dev/video0 --get-selection=crop       # 获取 crop 区域
v4l2-ctl -d /dev/video0 --get-selection=crop_bounds # 获取传感器原生尺寸
v4l2-ctl -d /dev/video0 --get-parm                 # 获取帧率参数
v4l2-ctl -d /dev/video0 --set-parm=30              # 尝试设置帧率

# ── 事件 ──
v4l2-ctl -d /dev/video0 --wait-event --event-type=all  # 等待并显示事件
```

### 8.2 常见问题排查

#### 问题 1：`VIDIOC_DQBUF` 返回 `-1` / `errno = EINVAL`

**原因**：通常在 `VIDIOC_REQBUFS` 之前尝试 DQBUF，或 buffer type/memory 与 REQBUFS 不一致。

**排查**：
```bash
# 确认 format 已设置
v4l2-ctl -d /dev/video0 --get-fmt-video
```

#### 问题 2：`VIDIOC_REQBUFS` 返回 `-1` / `errno = EBUSY`

**原因**：设备已被其他进程占用。

**排查**：
```bash
fuser /dev/video0         # 查看谁在使用
lsof /dev/video0
```

#### 问题 3：采集到的帧有绿色条纹或花屏

**原因**：
- 格式协商后 `sizeimage` 或 `bytesperline` 取错。
- 直接按 `width * height * 2` 计算 buffer 大小，但实际 `bytesperline` 大于 `width * 2`（有 stride）。
- 多平面格式用单平面 API 访问。

**修复**：始终使用 `VIDIOC_G_FMT` 返回的 `bytesperline` 和 `sizeimage`，不要自行计算。

#### 问题 4：帧率远低于预期

**逐步排查**：
```bash
# 1. 确认设备本身能达到的帧率
v4l2-ctl -d /dev/video0 --list-formats-ext | grep -A2 "1280x720"

# 2. 测试裸设备吞吐
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=300
# 注意输出的 fps

# 3. 检查曝光设置（低光环境自动曝光时间拉长会降低帧率）
v4l2-ctl -d /dev/video0 --list-ctrls | grep -i expo

# 4. 手动固定曝光时间恢复帧率
v4l2-ctl -d /dev/video0 --set-ctrl=exposure_auto=1   # Manual
v4l2-ctl -d /dev/video0 --set-ctrl=exposure_absolute=156  # 适当值

# 5. USB 带宽检查
lsusb -t | grep -i video  # 查看是否共享 USB controller
```

#### 问题 5：`select()` 指示有数据但 `VIDIOC_DQBUF` 立即返回错误

**原因**：可能是 buffer 全部在应用侧（用完忘了 QBUF），或者流已结束。

**检查**：
```bash
# 确认 STREAMING 状态
# 在代码中添加 buffer 水位日志
printf("available buffers: %d\n", buf.flags & V4L2_BUF_FLAG_QUEUED ? 1 : 0);
```

#### 问题 6：`mmap` 失败返回 `MAP_FAILED`

**原因**：
- `VIDIOC_QUERYBUF` 没调用或返回的 offset 错误。
- `mmap` 的 `length` 不够（应使用 `buf.length`）。
- fd 不是有效的 V4L2 设备（如 open 失败未检查）。

---

## 9. FFmpeg 中的 V4L2 集成

### 9.1 avdevice 封装的 V4L2 输入

FFmpeg 通过 `libavdevice` 的 `v4l2` input 封装了完整的 V4L2 MMAP 采集流程。使用方式：

```bash
# 基本采集
ffmpeg -f v4l2 -i /dev/video0 -c:v libx264 -preset ultrafast output.mp4

# 完整参数
ffmpeg \
  -f v4l2 \
  -input_format yuyv422 \
  -video_size 1280x720 \
  -framerate 30 \
  -ts monoatomic \          # timestamp 模式: default/monoatomic/abs
  -i /dev/video0 \
  -c:v libx264 \
  -preset ultrafast \
  -tune zerolatency \
  output.mp4
```

**时间戳选项说明**：
| `-ts` 值 | 行为 |
|-----------|------|
| `default` | 使用驱动提供的时间戳 |
| `monoatomic` | 强制 `CLOCK_MONOTONIC` |
| `abs` | 使用绝对时间 `CLOCK_REALTIME` |

### 9.2 在 C++ 代码中使用 FFmpeg 的 V4L2

在 StreamBridge 的 `linux/platform/capture/ffmpeg_video_capture.cpp` 中将采用类似方式：

```cpp
// 伪代码，展示 FFmpeg 封装 V4L2 的方式
AVFormatContext *fmt_ctx = nullptr;
AVInputFormat *v4l2_fmt = av_find_input_format("v4l2");

AVDictionary *options = nullptr;
av_dict_set(&options, "input_format", "yuyv422", 0);
av_dict_set(&options, "video_size", "1280x720", 0);
av_dict_set(&options, "framerate", "30", 0);

int ret = avformat_open_input(&fmt_ctx, "/dev/video0", v4l2_fmt, &options);
if (ret < 0) {
    // 错误处理：-ENOENT (设备不存在)、-EBUSY (被占用) 等
}

avformat_find_stream_info(fmt_ctx, nullptr);
// 循环：av_read_frame(fmt_ctx, pkt) → 送入编码器
```

### 9.3 原生 V4L2 vs FFmpeg 封装的选择

| 维度 | 原生 V4L2 ioctl | FFmpeg avdevice |
|------|----------------|-----------------|
| 控制粒度 | 完全控制（buffer 数、DMABUF、controls 等） | 有限，依赖 avdevice 封装 |
| 开发效率 | 低，大量样板代码 | 高，与 FFmpeg 管道无缝衔接 |
| 性能 | 最优（可避免 FFmpeg packet 分配） | 良好 |
| 可维护性 | 需自行处理所有错误路径 | FFmpeg 社区维护 |
| 跨平台 | Linux only | 理论上 FFmpeg 可跨平台 |
| 调试难度 | 高 | 中（FFmpeg 日志体系成熟） |

**StreamBridge 的决策**：第一阶段使用 FFmpeg 封装 V4L2（和 ALSA），以控制工作量；架构中保留原生 V4L2/ALSA 适配可能。

---

## 10. V4L2 与 Linux 内核子系统的关系

### 10.1 V4L2 Core (`drivers/media/v4l2-core/`)

内核中 V4L2 核心代码提供：
- **v4l2-dev.c**：字符设备注册、`/dev/videoN` 的 `file_operations`（open/close/ioctl/mmap/poll）
- **v4l2-ioctl.c**：所有标准 ioctl 的实现骨架→转调驱动回调
- **v4l2-ctrls.c**：control framework 核心（OR-ing/merging/clustering controls）
- **v4l2-mem2mem.c**：M2M 设备（编解码器/缩放器）的通用队列和调度
- **videobuf2/\*.c**：vb2 流式 I/O 框架（buffer 队列管理的复杂逻辑全在此）

### 10.2 videobuf2 (vb2) — buffer 管理的真相

vb2 是 V4L2 MMAP/USERPTR/DMABUF 的实际实现，驱动通常直接使用 vb2 提供的 helper 而不自己写 buffer 队列。

```text
应用调用:
ioctl(fd, VIDIOC_QBUF, &buf)
  └→ v4l2-ioctl.c → 驱动 .vidioc_qbuf()
       └→ vb2_ioctl_qbuf()          ← 绝大多数驱动都走这里
            └→ __vb2_queue_alloc()  ← 检查并加入 done_list
            └→ __enqueue_in_driver()

应用调用:
ioctl(fd, VIDIOC_DQBUF, &buf)
  └→ v4l2-ioctl.c → 驱动 .vidioc_dqbuf()
       └→ vb2_ioctl_dqbuf()
            └→ __vb2_get_done_vb()  ← 从 done_list 取出
            └→ vb2_buffer_done()    ← 转为应用可读状态
```

vb2 内部维护三个链表：
- **queued_list**：空 buffer，等待硬件填充
- **done_list**：已填充完毕，等待应用读取
- **active_list**：硬件处理中的 buffer

这是面试中深入理解 V4L2 "buffer 队列"的关键。

### 10.3 Media Controller

Media Controller 是 V4L2 的补充框架，用于描述复杂设备内部的 pipeline 拓扑。现代 SoC 的 camera pipeline 通常是一个有向无环图（DAG）。

```text
sensor ──► CSI-2 receiver ──► ISP ──► scaler ──► DMA /dev/video0
                                              └──► DMA /dev/video1
```

每个节点是 `media_entity`，连接通过 `media_pad` 和 `media_link`。应用通过 `/dev/mediaN` 枚举拓扑、配置链路使能和数据流向。

**实用场景**：在 Android Camera HAL3 中，用 Media Controller 配置从 sensor 到应用 buffer 的完整 pipeline。在嵌入式 Linux（Raspberry Pi、NVIDIA Jetson 等）中同样依赖此机制。

---

## 11. 进阶话题

### 11.1 V4L2 Request API（Linux 5.x+）

将多次配置（format + controls + buffer）打包成原子请求，确保所有参数在同一帧同时生效。这是配合无状态编解码器（stateless codecs，如 H.264 Annex B 解码）和 M2M 设备的重要机制。

### 11.2 色彩空间协商的陷阱

`colorspace` / `ycbcr_enc` / `quantization` / `xfer_func` 这组字段需要一致。常见错误：
- 摄像头输出 full-range BT.601，但下游按 limited-range BT.709 处理→颜色失真。
- 用 `ffmpeg` 时加 `-color_range 2 -colorspace bt709` 显式指定。

**默认约定**：SD 分辨率（≤576p）一般用 BT.601，HD（≥720p）用 BT.709。UVC 摄像头的 720p 输出通常已是 BT.709，但需要从 `V4L2_CID_COLORSPACE` control 确认。

### 11.3 帧同步与丢帧策略

在应用层实现软同步：

```cpp
// 伪代码：基于 CLOCK_MONOTONIC 的帧率控制
int64_t expected_frame_interval_us = 1'000'000 / target_fps;
int64_t prev_pts_us = -1;

while (running) {
    int64_t pts_us = capture_and_get_pts_us();

    if (prev_pts_us >= 0) {
        int64_t measured_interval_us = pts_us - prev_pts_us;

        if (measured_interval_us < expected_frame_interval_us * 0.8) {
            // 帧到达过快（可能是时间戳异常或驱动暴增帧率）
            log_dropped_frame();
            continue;
        }
        if (measured_interval_us > expected_frame_interval_us * 2.5) {
            // 掉帧，记录但不丢弃
            log_frame_gap(measured_interval_us);
        }
    }
    prev_pts_us = pts_us;
    process_frame_and_encode();
}
```

### 11.4 安全关闭

错误的关闭顺序可能导致 `munmap` 时 kernel panic 或 `VIDIOC_STREAMOFF` 死等在 DMA 完成上。安全顺序：

```cpp
// 1. 停止采集循环
running = false;

// 2. 取消可能阻塞的 DQBUF（可选：非阻塞模式 + 信号中断）
//    或使用 select() 超时管理

// 3. 流关闭
int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_STREAMOFF, &type);

// 4. 清空驱动队列（可选但安全）
struct v4l2_buffer buf = {0};
buf.type   = type;
buf.memory = V4L2_MEMORY_MMAP;
while (ioctl(fd, VIDIOC_DQBUF, &buf) == 0) {
    // 丢弃残留 frame
}

// 5. 释放用户空间映射
for (auto &b : buffers) {
    munmap(b.start, b.length);
}

// 6. 释放内核 buffer
struct v4l2_requestbuffers req = {0};
req.type   = type;
req.memory = V4L2_MEMORY_MMAP;
req.count  = 0;
ioctl(fd, VIDIOC_REQBUFS, &req);

// 7. 关闭设备
close(fd);
```

---

## 12. 参考资料

- [Linux Kernel V4L2 Documentation](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html) — 内核官方文档
- [V4L2 API Specification](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/vidioc-g-fmt.html) — 所有 ioctl 参考
- [Linux Media Infrastructure API](https://www.kernel.org/doc/html/latest/userspace-api/media/media.html) — Media Controller + V4L2 Subdev
- `v4l2-ctl --help-all` — 命令行工具完整帮助
- `videobuf2` 源码：`drivers/media/common/videobuf2/` — 理解 buffer 管理的最佳资源
- `include/uapi/linux/videodev2.h` — 所有数据结构和 ioctl 编号定义

---

> 本文档为 StreamBridge 项目 Linux 采集端参考文档。与 `docs/timestamp-and-av-sync.md` 配合阅读，理解从 V4L2 时间戳到统一时间基的完整转换链路。
