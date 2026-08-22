# DMA 与 mmap 详解（V4L2、Linux 音视频开发）

## 一、先记住一句话

很多人刚学习 V4L2 时，最容易混淆 DMA 和 mmap。

实际上，它们解决的是两个完全不同的问题：

* **DMA（Direct Memory Access）负责：数据如何进入内存。**
* **mmap（Memory Mapping）负责：程序如何访问这块内存。**

因此：

> **DMA ≠ mmap，它们通常配合使用，但彼此独立。**

---

# 二、DMA 是什么？

DMA（Direct Memory Access，直接内存访问）是一种硬件数据传输机制。

它允许外设（Camera、网卡、SSD、GPU 等）**直接与内存交换数据，而不需要 CPU 一字节一字节搬运。**

例如摄像头采集一帧图像：

没有 DMA：

```text
Camera
   │
   ▼
CPU
   │
   ▼
Memory
```

CPU 必须不停执行：

```cpp
buffer[i] = camera_read();
```

如果：

* 1080P
* YUV420
* 30fps

每秒需要搬运约 90MB 数据。

CPU 会浪费大量时间。

---

有 DMA：

```text
Camera
   │
   ▼
DMA Controller
   │
   ▼
Memory
```

CPU 只负责：

1. 配置 DMA
2. 指定目标地址
3. 等待 DMA 完成
4. 收到完成中断

真正的数据搬运全部由 DMA Controller 完成。

---

# 三、mmap 是什么？

mmap（Memory Mapping）是一种内存映射机制。

它可以把：

* 文件
* 设备内存
* 共享内存

映射到当前进程的虚拟地址空间。

例如：

```cpp
void* ptr = mmap(...);
```

以后：

```cpp
((uint8_t*)ptr)[100];
```

就可以像访问普通数组一样访问那块内存。

---

## mmap 不会复制数据

这是最重要的一点。

很多人误认为：

```text
Kernel Buffer

↓

mmap

↓

User Buffer
```

其实这是错误的。

真正发生的是：

```text
User Virtual Address
        │
        ▼
 Physical Memory
        ▲
        │
Kernel Virtual Address
```

Kernel 和 User 只是拥有了**不同的虚拟地址**，但最终访问的是**同一块物理内存**。

因此：

**mmap 本身没有任何 memcpy。**

---

# 四、为什么需要 mmap？

Linux 内核有严格的地址空间隔离：

```text
+-------------------------+
|       User Space        |
|                         |
|      Application        |
+-------------------------+

        不能直接访问

+-------------------------+
|      Kernel Space       |
|                         |
|    Camera Buffer        |
+-------------------------+
```

如果没有 mmap：

应用程序只能：

```cpp
read(fd, user_buffer, size);
```

实际发生的是：

```text
Kernel Buffer

↓

memcpy

↓

User Buffer
```

如果每秒几十 MB、几百 MB 数据，这次 memcpy 会占用大量 CPU。

mmap 的目的就是：

**让用户程序直接访问 Kernel Buffer，而不再复制一次。**

---

# 五、DMA 和 mmap 的关系

二者解决的是不同的问题。

完整流程如下：

```text
Camera
   │
   ▼
DMA
   │
   ▼
Kernel Buffer
   │
   ▼
mmap
   │
   ▼
User Space
```

其中：

DMA：

负责：

> Camera → Kernel Buffer

mmap：

负责：

> Kernel Buffer → User Space（建立映射，不复制）

---

# 六、如果没有 DMA，会发生什么？

很多人以为：

没有 DMA 就不能 mmap。

这是错误的。

没有 DMA：

流程变成：

```text
Camera
   │
   ▼
CPU
   │
   ▼
Kernel Buffer
   │
   ▼
mmap
   │
   ▼
User Space
```

区别只是：

Camera 数据由 CPU 自己搬运到 Kernel Buffer。

而 mmap 仍然可以把 Kernel Buffer 映射给用户程序。

所以：

**DMA 与 mmap 没有依赖关系。**

---

# 七、如果没有 mmap，会发生什么？

即使有 DMA：

```text
Camera
   │
   ▼
DMA
   │
   ▼
Kernel Buffer
```

用户程序仍然不能直接访问。

于是：

```text
Kernel Buffer

↓

memcpy

↓

User Buffer
```

又产生了一次 CPU 拷贝。

---

# 八、四种组合

DMA 和 mmap 可以自由组合。

## ① 没有 DMA，没有 mmap

```text
Camera

↓

CPU

↓

Kernel Buffer

↓

CPU memcpy

↓

User Buffer
```

CPU：

负责两次数据搬运。

性能最差。

---

## ② 没有 DMA，有 mmap

```text
Camera

↓

CPU

↓

Kernel Buffer

↓

mmap

↓

User Space
```

CPU：

只负责：

Camera → Kernel。

Kernel → User 没有 memcpy。

---

## ③ 有 DMA，没有 mmap

```text
Camera

↓

DMA

↓

Kernel Buffer

↓

CPU memcpy

↓

User Buffer
```

Camera → Kernel 不需要 CPU。

Kernel → User 仍然需要 memcpy。

---

## ④ 有 DMA，有 mmap（最佳方案）

```text
Camera

↓

DMA

↓

Kernel Buffer

▲

│

mmap

│

User Space
```

Camera → Kernel：

没有 CPU。

Kernel → User：

没有 memcpy。

这是现代 Camera 最常见的方案。

---

# 九、V4L2 为什么采用 DMA + mmap？

V4L2 的 MMAP 模式本质上就是：

第一步：

驱动申请 Kernel Buffer：

```cpp
VIDIOC_REQBUFS
```

第二步：

Camera 使用 DMA 写入 Buffer：

```text
Camera

↓

DMA

↓

Kernel Buffer
```

第三步：

应用调用：

```cpp
mmap(...)
```

建立用户空间映射：

```text
Kernel Buffer

⇅

User Pointer
```

第四步：

程序通过：

```cpp
VIDIOC_DQBUF
```

拿到 Buffer Index：

```cpp
buffers_[buf.index]
```

直接访问图像数据。

整个过程中：

没有：

```cpp
memcpy()
```

因此 CPU 开销非常低。

---

# 十、DMA 和 memcpy 的区别

memcpy：

```text
CPU

↓

Load

↓

Store

↓

Load

↓

Store
```

CPU 一直参与搬运。

DMA：

```text
CPU

↓

配置 DMA

↓

DMA 自己搬运

↓

DMA 完成中断

↓

CPU 继续处理
```

CPU 基本不参与数据传输。

---

# 十一、DMA-BUF 又是什么？

DMA-BUF 是 Linux 提供的一种 Buffer 共享机制。

例如：

```text
Camera

↓

DMA-BUF

├── GPU

├── Video Encoder

├── Display

└── ISP
```

多个硬件共享同一块物理内存。

不需要：

```text
Camera

↓

CPU memcpy

↓

GPU
```

Android 中：

AHardwareBuffer、

EGLImage、

MediaCodec

很多底层最终都会使用 DMA-BUF。

---

# 十二、面试高频问题

### Q1：DMA 和 mmap 有什么区别？

DMA 负责把数据搬运到内存，是一种硬件传输机制；mmap 负责建立用户空间与内核空间之间的虚拟地址映射，让程序可以直接访问 Kernel Buffer。两者解决的是不同问题。

---

### Q2：为什么 mmap 不需要 memcpy？

因为 mmap 建立的是虚拟地址映射。

Kernel 和 User 拥有不同的虚拟地址，但最终访问的是同一块物理内存，所以不存在数据复制。

---

### Q3：为什么 V4L2 使用 DMA + mmap？

DMA 消除了 Camera → Kernel 的 CPU 拷贝；

mmap 消除了 Kernel → User 的 CPU 拷贝。

两者结合，可以实现整个采集过程没有 CPU 数据搬运，是 V4L2 高性能采集的核心。

---

# 十三、一张图总结

```text
                没有优化

Camera
   │
   ▼
CPU（搬数据）
   │
   ▼
Kernel Buffer
   │
CPU memcpy
   ▼
User Buffer


======================================


              V4L2 推荐方案

Camera
   │
   ▼
DMA（负责搬数据）
   │
   ▼
Kernel Buffer
      ▲
      │
      │ mmap（建立地址映射）
      │
      ▼
User Space
```

可以把整个过程理解成：

* **DMA：负责"搬数据"。**
* **mmap：负责"共享这块内存"。**

两者配合，构成了现代 Linux Camera、GPU、MediaCodec、网卡、SSD 等高性能设备最常见的数据传输方式。



# 十四、为什么 DMA 比 CPU 搬运数据效率高？

很多初学者都会有一个疑问：

> **数据总要搬运，最终还是经过 CPU 吧？为什么 DMA 会更快？**

这里有一个关键误区：

**DMA Controller 和 CPU 是两套独立的硬件，它们都可以访问内存（DDR），但 DMA 的数据搬运过程不需要 CPU 参与。**

---

## 1. 没有 DMA

如果没有 DMA，Camera 无法直接把数据写入内存。

CPU 必须不断从 Camera 的数据寄存器（或 FIFO）读取数据，再写入 Kernel Buffer。

流程如下：

```text id="2cw8gg"
Camera
   │
   ▼
Camera FIFO
   │
   ▼
CPU（不停读取、写内存）
   │
   ▼
Kernel Buffer
```

CPU 大致需要执行如下逻辑：

```cpp id="dc8kh9"
for (...) {
    kernel_buffer[i] = camera_read();
}
```

整个采集过程中，CPU 都在充当"搬运工"。

如果是一帧 3MB、30fps 的视频，每秒需要搬运约 90MB 数据，CPU 会消耗大量时间在数据传输上，而无法专注于编码、渲染、网络等真正的计算任务。

---

## 2. 有 DMA

有 DMA 后，CPU 只需要在开始采集时配置一次 DMA：

```text id="gjvbna"
源地址：Camera FIFO

目标地址：Kernel Buffer

长度：3MB
```

随后：

```text id="l0o1tr"
Camera
   │
   ▼
DMA Controller
   │
   ▼
Kernel Buffer
```

真正的数据搬运由 DMA Controller 自动完成。

CPU 此时可以同时执行：

* H.264/H.265 编码
* OpenGL 渲染
* 网络发送
* AI 推理
* 其它业务逻辑

等 DMA 完成后，只会收到一次完成中断，然后继续处理下一步。

---

## 3. DMA 的优势到底是什么？

很多人认为 DMA 的优势是：

> **搬运速度比 CPU 更快。**

其实更准确的说法应该是：

> **DMA 的核心优势是让 CPU 和数据传输可以并行工作。**

例如：

```text id="d09mtz"
                DDR

               ▲     ▲
              /       \

      CPU               DMA
       │                 │
       │                 │
H264 编码         Camera 数据搬运
```

如果没有 DMA：

```text id="rz55a4"
CPU

↓

搬数据

↓

编码

↓

搬数据

↓

编码
```

两项工作只能串行执行。

有了 DMA：

```text id="cmps2m"
CPU

↓

编码（同时）

DMA

↓

搬数据
```

CPU 和 DMA 可以同时工作，大幅提升系统整体吞吐率。

---

## 4. 为什么现代音视频系统都使用 DMA？

Camera、GPU、网卡、SSD、Audio 等设备，每秒都需要传输几十 MB、几百 MB，甚至 GB 级的数据。

如果全部依赖 CPU 搬运：

CPU 将长期处于高负载状态。

因此现代系统几乎都采用：

```text id="jx9g4m"
外设

↓

DMA

↓

Kernel Buffer

↓

mmap / DMA-BUF

↓

User / GPU / Encoder
```

这种架构实现高吞吐、低延迟和低 CPU 占用，也是 Linux、Android 音视频系统实现零拷贝（Zero Copy）的基础。
