# StreamBridge Android 端问题排查记录

## 问题 1：APK 安装失败 — INSTALL_FAILED_USER_RESTRICTED

**现象**：`adb install` 报 `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`

**原因**：小米手机（MIUI）默认禁止 USB 安装。需要在开发者选项中开启"USB 安装"或"通过 USB 安装应用"。

**解决**：用户在手机上允许 USB 安装权限后解决。

**面试要点**：Android 设备管理策略，不同厂商（MIUI/ColorOS/EMUI）对 USB 调试的限制差异。

---

## 问题 2：App 闪退 — DT_NEEDED 嵌入 Windows 绝对路径

**现象**：
```
UnsatisfiedLinkError: dlopen failed: library "D:\code\StreamBridge\android\app\build\intermediates\..."
not found: needed by libstreambridge_android.so
```

**根因**：
1. FFmpeg 交叉编译时，`.a` → `.so` 的链接步骤**没有设置 DT_SONAME**（即 `-Wl,-soname,libavcodec.so`）
2. ndk-build 的 `PREBUILT_SHARED_LIBRARY` 会将预编译 `.so` 复制到 NDK 中间目录
3. 链接 `libstreambridge_android.so` 时，由于 FFmpeg `.so` 缺少 SONAME，链接器**将 Windows 绝对路径写入了 ELF 的 DT_NEEDED 字段**
4. Android 设备上的动态链接器 `dlopen()` 找不到这个 Windows 路径 → 抛 UnsatisfiedLinkError

**验证方法**：
```bash
llvm-readobj --dynamic-table libstreambridge_android.so | grep NEEDED
# 错误：NEEDED Shared library: [D:\code\StreamBridge\...\libavformat.so]
# 正确：NEEDED Shared library: [libavformat.so]
```

**修复**：用 `.a` 文件重新链接 `.so`，加上 `-Wl,-soname,libavXXX.so`

**面试要点**：ELF 文件格式、动态链接过程、DT_SONAME / DT_NEEDED 的含义、Android 的动态链接器 vs GNU ld.so。

---

## 问题 3：libavformat.so 缺少 zlib 符号

**现象**：
```
UnsatisfiedLinkError: dlopen failed: cannot locate symbol "uncompress"
referenced by libavformat.so
```

**原因**：FFmpeg RTMP 实现内部使用 zlib 进行握手数据压缩/解压。`.so` 链接时没有链接 `-lz`。

**修复**：重新链接时添加 `-lz`。`libz.so` 是 Android 系统自带库，无需打包进 APK。

**面试要点**：FFmpeg 模块间依赖关系（RTMP → zlib），Android 系统库 vs 应用打包库。

---

## 问题 4：RTMP 连接失败 — EADDRNOTAVAIL

**现象**：`avformat_open_input failed: Cannot assign requested address`，但 `nc` 可连。

**排查过程**：

| 步骤 | 测试内容 | 结果 | 结论 |
|------|----------|------|------|
| 1 | SRS 可用性 | Windows ffplay 可播 | SRS + 推流端正常 |
| 2 | 手机 ping Linux | 通 | 网络层连通 |
| 3 | adb shell nc 连接 1935 | 通 | TCP 端口可达 |
| 4 | App 内 Java Socket | 通 | 进程级网络权限正常 |
| 5 | 加 ACCESS_NETWORK_STATE + bindProcessToNetwork | 绑成功但仍失败 | 不是网络选择问题 |
| 6 | Native POSIX socket() + connect() | 通 | 原生 socket API 可用 |
| 7 | **去掉 avformat_open_input 的所有选项** | **通了！** | 选项是根因 |

**根因**：`av_dict_set(&opts, "timeout", ...)` 和 `av_dict_set(&opts, "rtmp_live", "live")` 在 FFmpeg 7.0.2 最小化构建 + Android 环境下，导致内部 TCP 连接流程异常。

**修复**：去掉所有 `av_dict_set` 选项，`avformat_open_input` 传 `nullptr`。

**面试要点**：
- 分层定位法：应用层 → 系统调用层 → 库层，逐层排除
- FFmpeg 选项系统的黑盒风险：不同平台/构建配置下，相同选项可能有不同行为
- 最小化复现原则：去掉所有非必要参数，用最原始 API 测试

---

## 问题 5：Gradle 构建 — JDK 环境

**现象**：本机无 Java，`gradlew.bat` 无法运行。

**解决**：使用 Android Studio 自带的 JBR (JetBrains Runtime)：
```
JAVA_HOME=D:/soft/AS/jbr  # JDK 25
```

---

## 问题 6：视频 PTS 始终为 0——时间基不匹配 + H.264 解码器 PTS 不透传

**现象**：
- 视频帧正常解码渲染（~33fps 1280x720），`pts=0` 始终为 0
- 首帧 PTS 归一化后所有帧 PTS 变 0，画面时间戳失效

**排查过程**：

| 步骤 | 测试内容 | 结果 | 结论 |
|------|----------|------|------|
| 1 | 订阅器打印原始 PTS | `video raw pkt#1 pts=10 tb=1/1000` | FLV PTS 毫秒，正常递增 |
| 2 | 解码器 time_base | `codec_tb=1/1000` | 保留流原始时间基 |
| 3 | 发送前 PTS 转换 | `to_codec_tb(10000us, {1,1000})` = 10 | 微秒→毫秒转换正确 |
| 4 | 视频解码器 av_frame->pts | `frame#0 av_pts=10, frame#1 av_pts=10, ...` | **所有帧 PTS 都是 10！** |
| 5 | 音频 PTS 对比 | `av_pts=671 pts_us=671000` 正常递变 | 排除订阅器问题 |

**根因（双层）**：

1. **时间基单位不匹配**：订阅器输出统一微秒（`packet.pts.us = 10000` 即 10ms），但解码器 `time_base = {1, 1000}` 毫秒。直接传入 `avpkt->pts = 10000`，解码器按毫秒理解就是 10 秒——被当成无效时间戳丢弃。

2. **H.264 解码器的 PTS 不透明透传**：与 AAC 不同，H.264 解码器会缓冲多个包才产出帧。即使修复了 time_base 转换，所有输出帧的 `av_frame->pts` 都固定为第一个包的 PTS（10ms），解码器内部对 PTS 的处理是"粘性"的。

**修复（三层方案）**：

第一层：时间基转换
```cpp
// 发前：微秒 -> codec time_base
avpkt->pts = av_rescale_q(packet.pts.us, {1, 1000000}, codec_ctx_->time_base);
// 收后：codec time_base -> 微秒
frame.pts.us = av_rescale_q(av_frame->pts, codec_ctx_->time_base, {1, 1000000});
```

第二层：自维护 PTS 队列（FFmpeg 解码标准范式）
```cpp
std::deque<int64_t> pts_queue_;  // 微秒值 FIFO

// 每发一个包 push 一次
pts_queue_.push_back(packet.pts.us);
avcodec_send_packet(ctx, pkt);

// 每收一帧 pop 一次
avcodec_receive_frame(ctx, frame);
frame.pts = TimePointUs{pts_queue_.front()};
pts_queue_.pop_front();
```

第三层：send_packet / receive_frame API 分离
```cpp
// send_packet() — 只发不接，push PTS 一次
// receive_frame() — 只接不发，pop PTS 一次
// 调用方：send 一次 → receive 循环直到 EAGAIN
```
这个设计避免了调用方在循环中误调 `decode()` 导致重复发包（见问题 7）。

**面试要点**：
- FFmpeg time_base 概念：流时间基 vs 编解码器时间基 vs 统一时间单位
- `av_rescale_q`：在不同时间基之间做精确有理数转换，避免浮点精度损失
- H.264 vs AAC 解码器的 PTS 行为差异（AAC 帧边界明确，H.264 需要缓冲）
- PTS 队列模式是视频解码的标准范式（类似 MediaCodec 的 `queueInputBuffer` / `dequeueOutputBuffer`）
- FLV/RTMP 的时间基通常是 {1,1000}（毫秒），MPEG-TS 是 {1,90000}，必须查询流信息
- AAC 编码器延迟（priming samples）：首个解码帧 PTS 远超首个包 PTS（~546ms）

---

## 问题 7：音频解码重复发包——所有帧 PTS 相同、声音异常

**现象**（接问题 6，视频 PTS 修复后）：
- 视频完美，PTS 正常递增
- 音频诊断日志：
  ```
  frame#0  pts_us=546000 bytes=00000000
  frame#50 pts_us=546000 bytes=03019801
  frame#100 pts_us=546000 bytes=03019801
  frame#150 pts_us=546000 bytes=03019801
  ```
- **所有帧 PTS 和数据完全相同！** 声音嗡嗡响或嘀嘀嘀

**排查过程**：

| 步骤 | 观察 | 结论 |
|------|------|------|
| 1 | 视频 PTS 正常递增（10000→50000→90000...） | 排除订阅器问题 |
| 2 | 音频帧首帧全零，后续帧数据完全相同 | 解码器产出相同帧 |
| 3 | 检查音频解码循环代码 | 发现 inner while 循环重复调用 `decode(packet)` |
| 4 | 确认：`decode()` = send + receive，循环中 send 执行 N 次 | **根因确认！** |

**根因**：

音频解码循环与视频有**相同的结构缺陷**：
```cpp
// bug 代码
while (pop packet) {
    while (true) {
        auto result = audio_decoder_.decode(packet);  // 同一个 packet 被反复发送！
        if (!result->has_frame) break;
        // write to AAudio
    }
}
```

每次内层循环迭代，`decode()` 内部执行：
```cpp
pts_queue_.push_back(packet.pts.us);  // 同一个 PTS push N 次
avcodec_send_packet(ctx, same_data);  // 同一个数据 send N 次
avcodec_receive_frame(ctx, frame);    // 每次收到相同的帧！
pts_queue_.pop_front();               // pop 出同一个 PTS
```

**为什么 AAC 解码器不拒绝？**
H.264 解码器在 buffer 满时返回 `EAGAIN` 拒绝多余数据，阻止了无限重复。但 AAC 解码器接受重复输入并反复产出相同帧——不同编解码器实现的行为差异。

**修复**：与视频解码器一致，API 从 `decode()`（send+receive 合体）拆分为 `send_packet()` + `receive_frame()`：
```cpp
// 修复后
while (pop packet) {
    audio_decoder_.send_packet(packet);   // 只发一次
    
    while (true) {
        auto result = audio_decoder_.receive_frame();  // 只收不發
        if (!result->has_frame) break;
        // write to AAudio
    }
}
```

**面试要点**：
- send/receive 分离是 FFmpeg 解码的标准范式，**send 只能调用一次**
- 不同编解码器对错误输入的容错行为不同——H.264 返回 EAGAIN，AAC 静默接受
- "API 误用"类 bug：`decode()` 既是 send 又是 receive，在循环中调用不可避免重复发送
- PTS 队列必须与 send 1:1 绑定（push 在 send_packet 内，pop 在 receive_frame 内）
- 诊断手法：周期性采样日志（每 50 帧），发现规律性重复值即可锁定此类 bug

---

## 经验教训

1. **交叉编译产物必须验证 ELF 元数据**：检查 SONAME、NEEDED、依赖库等动态段信息
2. **`--disable-all` 有风险**：FFmpeg 最小化构建容易遗漏隐藏依赖（RTMP → zlib）
3. **Android 动态库加载顺序**：按拓扑序（avformat → avcodec → swresample → avutil）
4. **Windows + NDK 交叉编译的路径陷阱**：Windows 绝对路径可能被嵌入 ELF
5. **FFmpeg 解码务必使用 send/receive 分离模式**：不要图方便把 send 和 receive 封装在同一个函数里在循环中调用，这会导致重复发包——视频（H.264）和音频（AAC）都中过这个坑
6. **PTS 不要依赖 av_frame->pts**：解码器对 PTS 的处理是不透明的，自己维护 FIFO 队列是唯一可靠方案
