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

## 问题 8：画面顶部正常，下方被拉成长条——H.264 多 NAL 未完整转 Annex-B

**现象**：
- Linux 推流端确认正常。
- Android 播放端能出画面，但只有画面顶部一小条正常，下面大面积变成顶部内容向下延伸的竖向长条。
- 切到 FFmpeg 软件解码后仍然一样，排除单纯 MediaCodec Surface 输出问题。
- UI 状态里可能显示 `sync=Wait` 或 `av_diff_us=...`，但这不是根因。

**第一判断**：

这种现象很像 stride/linesize 错误，例如：
- 把 `ANativeWindow_Buffer.stride` 当字节而不是像素；
- 把 FFmpeg `AVFrame.linesize[]` 错当 `width`；
- RGBA 每行按 `width * 4` 写入，但目标 buffer 有 padding。

因此先检查渲染日志：

```text
render cpu 1280x720 data=... stride=5120
first render: src=1280x720(stride=5120) buf=1280x720(stride=1280)
```

这个日志说明：
- 源 RGBA stride = `1280 * 4 = 5120` 字节，正常；
- `ANativeWindow_Buffer.stride = 1280` 像素，写入时使用 `uint32_t*`，正常；
- renderer 层没有明显行跨度错误。

**真正根因**：

RTMP/FLV 中的 H.264 使用 avcC 格式时，视频 packet 不是 Annex-B start code 分隔，而是长度前缀格式：

```text
[length][NAL][length][NAL][length][NAL]...
```

原代码只把 packet 的前 4 字节改成 start code：

```cpp
packet.data[0] = 0x00;
packet.data[1] = 0x00;
packet.data[2] = 0x00;
packet.data[3] = 0x01;
```

这只转换了第一个 NAL，后续 NAL 仍然保留 length prefix。一个视频帧如果由多个 slice/NAL 组成，解码器只能正确解出前面一部分 slice，后续 slice 损坏，于是 H.264 错误隐藏会把最后有效区域向下扩展，最终画面看起来像“上面正常、下面被拉长”。

**修复**：

从 avcC/hvcC extradata 解析 `nal_length_size`，然后对每个 video packet 完整扫描所有 NAL：

```text
[len][NAL][len][NAL]...
↓
[00 00 00 01][NAL][00 00 00 01][NAL]...
```

修复文件：
- `common/src/ffmpeg/ffmpeg_subscriber.h`
- `common/src/ffmpeg/ffmpeg_subscriber.cpp`

核心逻辑：

```cpp
while (offset + nal_length_size <= data.size()) {
    nal_size = read_be_length(data + offset, nal_length_size);
    offset += nal_length_size;

    append 00 00 00 01;
    append data[offset : offset + nal_size];
    offset += nal_size;
}
```

同时保留 renderer 稳定化修改：
- `ANativeWindow_setBuffersGeometry(window, frame.width, frame.height, RGBA_8888)`
- RGBA/BGRA/YUV420P 都按 plane stride 和 window stride 逐行写入，避免额外 letterbox 缩放路径干扰排查。

**验证日志**：

修复后应看到：

```text
subscriber: Annex-B conversion enabled (container=avcC nal_len_size=4)
annexb pkt#... converted ... -> ... bytes
```

用户真机验证：画面恢复正常。

**面试要点**：
- H.264 有两种常见封装形式：Annex-B start code 与 avcC length-prefixed。
- FLV/MP4 packet 内可能包含多个 NAL，不能只替换第一个 length prefix。
- “画面局部正常、其余区域拖影/拉伸”不一定是渲染 stride，也可能是码流 slice 不完整触发解码错误隐藏。
- 排查顺序：先用日志确认源 stride 与目标 stride，再检查 bitstream 格式转换。
- MediaCodec 和 FFmpeg 软件解码都异常时，应优先怀疑解码前的 packet 数据。

---

## 经验教训

1. **交叉编译产物必须验证 ELF 元数据**：检查 SONAME、NEEDED、依赖库等动态段信息
2. **`--disable-all` 有风险**：FFmpeg 最小化构建容易遗漏隐藏依赖（RTMP → zlib）
3. **Android 动态库加载顺序**：按拓扑序（avformat → avcodec → swresample → avutil）
4. **Windows + NDK 交叉编译的路径陷阱**：Windows 绝对路径可能被嵌入 ELF
5. **FFmpeg 解码务必使用 send/receive 分离模式**：不要图方便把 send 和 receive 封装在同一个函数里在循环中调用，这会导致重复发包——视频（H.264）和音频（AAC）都中过这个坑
6. **PTS 不要依赖 av_frame->pts**：解码器对 PTS 的处理是不透明的，自己维护 FIFO 队列是唯一可靠方案
7. **avcC/hvcC 转 Annex-B 必须逐 NAL 完整转换**：一个 packet 可能包含多个 NAL，只替换前 4 字节会造成部分 slice 损坏，表现可能像渲染拉伸。
8. **音视频时间戳必须共用一个时钟域**：采集/编码任何一环用合成计数器 PTS，或编码器把 PTS 归零到自己的时间轴，都会导致音画偏移且随丢帧漂移（见问题 11）。

---

## 问题 9：MediaCodec zero-copy 能出帧，但画面停在前几帧不动

**现象**：

- 日志能看到硬解路径已经启用：

```text
using MediaCodec hardware decoder (zero-copy surface output)
configure zero-copy: surface=...
video decoder opened, mode=MediaCodec zero-copy
```

- 前几帧有输出：

```text
StreamBridgeMC frame#1 pts=10000 idx=15
video: frame#1 pts=0 av=0 act=Render
```

- 随后队列接近满并持续丢帧：

```text
video: HEARTBEAT pkt_fed=11 frame_out=11 dropped=8 vq=59 aq=49 has_audio=1 sync=Drop
```

**根因**：

这不是 MediaCodec 没有解码，也不是 Surface 没有 present。真正问题是视频线程在每个 packet 后调用阻塞式 drain：

```cpp
receive_frame(200);
```

当硬解暂时没有 ready 输出时，每个视频 packet 最多阻塞 200ms。30fps 视频本应约 33ms 一个 packet，结果喂包速度被拖到远低于实时速度，视频队列堆满，音频主时钟继续前进，AV sync 判断视频严重落后，于是持续 `Drop`。画面就停在最后一次真正 present 的帧上。

**同时发现的放大因素**：

启动后 Java/Surface 回调可能再次传入同一个 `ANativeWindow`。如果不判断是否同一个 Surface，会触发一次不必要的 MediaCodec recreate，进一步增加启动阶段延迟。

**修复**：

- 相同 Surface 不再调用 `MediaCodecVideoDecoder::set_surface()`，避免无意义 recreate。
- 视频 drain 改成非阻塞：

```cpp
receive_frame(0);
```

这样每轮只取已经 ready 的输出帧，不因为暂时没帧而阻塞后续 packet 输入。

**验证重点**：

- `fed pkt#` 应持续快速增长，不应启动几秒后只到十几个。
- `vq` 不应长期贴近容量上限，例如 `59/60`。
- `sync=Drop` 可以短暂出现，但不应长时间连续。
- 画面应持续更新。

---

## 问题 10：画面一会正常，一会马赛克，并且逐渐变糊

**现象**：

- 视频可以播放，但画面稳定性差。
- 偶尔某一帧看起来正常，随后又逐渐出现花屏、马赛克、糊成块。
- 等到下一个关键帧附近可能短暂恢复，然后再次变差。

**根因**：

压缩视频 packet 队列满时丢了旧 packet。`MediaQueue::push()` 原本在 `ByCount` 队列满时会自动 `drop_oldest`，这对已解码的 RGBA frame 还能接受，但对 H.264/AAC 压缩 packet 是危险的。

H.264 的 P 帧依赖前面的参考帧。只要中间丢了一个压缩 packet，后续 P 帧参考链就断了，解码器只能错误隐藏，于是表现为马赛克、拖影、逐渐变糊。关键帧到来后参考链重建，画面才可能短暂恢复。

**修复**：

- `MediaQueue::Config` 增加 `drop_oldest_on_full`。
- Android 播放端的音视频压缩 packet 队列设置为 `drop_oldest_on_full = false`。
- 如果队列堵塞超过 `kDemuxReadTimeoutMs`，不再静默丢当前 packet，而是记录日志并走重连恢复。

**规则**：

- 解码前的 H.264/AAC packet 不允许为了追实时随便丢。
- 需要追实时，只能：
  - 解码后丢已经输出的 video frame；
  - 或者显式 flush decoder，等待下一个 keyframe/I 帧重新同步；
  - 或者让推流端降低码率、分辨率、fps、GOP 长度。

**验证重点**：

- 队列满时不应看到压缩 packet 被静默丢弃。
- 花屏/马赛克应明显减少或消失。
- 如果设备确实解不过来，应看到队列阻塞和重连日志，而不是持续播放损坏参考链。

---

## 问题 11：Linux 推流端 camera+ALSA 音画不同步（文件源推流正常）

**现象**：

- 用本地文件推流（`--video-backend file`）音画同步正常；
- 换成 V4L2 摄像头 + ALSA 麦克风后音画不同步，且随播放时间推移越来越严重；
- 录制的 FLV 文件里音频 PTS 序列每 15 个包出现规律性 ~350ms 空洞。

**根因（三层叠加）**：

1. **采集 PTS 用 `frame_idx × 名义时长` 合成**：V4L2 与 ALSA 各自从 0 起算，两路启动时刻差变成恒定唇音偏移；任一路丢帧/XRUN 后合成时钟与真实时间永久漂移。文件源两路共享同一 demux 时间线所以正常。
2. **音频编码器 PTS 与输入脱钩**：`FFmpegAudioEncoder` 用内部计数器生成输出 PTS（无视输入帧的真实采集时刻），音频时间轴被归零，与视频绝对时钟完全错位；且 960 帧 ALSA period 与 1024 AAC 帧互质，累积缓冲每 16 chunk 恰好清空重启，若用**全局**帧计数做 PTS 偏移，重启后会把上一轮的帧数重复叠加——这就是 PTS 周期性跳变 ~350ms 的来源。
3. **mux 无跨流起始对齐**：两路绝对 PTS 直接交织。初版对齐器「对齐前立即丢弃单路包」是错的——33ms/21ms 的到达网格不同步，两路首包几乎永远不同时在队列里，对齐被饿死直到 3s 超时；期间旁路阻塞 pop 还会把低于零点的包直接写出（负 PTS 导致 FLV muxer EINVAL）。

**修复**：

1. 采集 PTS 统一 `CLOCK_MONOTONIC`：V4L2 优先驱动 `buf.timestamp`（uvcvideo 在帧完成时打点，本机摄像头不提供则回退到出队时刻）；ALSA 用 `snd_pcm_readi` 完成时刻。两路必须共用同一时间域。
2. 音频编码 PTS = `累积器首采样采集时刻 + 本轮帧偏移 × 帧长`，帧偏移在累积器每轮重启时归零（不能复用全局帧计数）。
3. 新增公共纯逻辑类 `AvStartAligner`（`common/include/streambridge/av_start_aligner.h`）：对齐前**只等待不丢弃**，双路首包齐备后 `base = max(两路首包)`，低于 base 的包丢弃（较早启动那一路的无效启动期数据），其余统一平移；3s 超时保护（对端设备故障时强制对齐）。mux 对齐前不消费任何包。

**验证**：

- 自动化：`ctest -R av_sync` —— `av_sync_capture_test` 走生产链路录制 10s 到 `source/av_sync_test.flv` 并离线分析（判定阈值见 `docs/testing-guide.md`）。修复后实测：start skew +20ms、drift +9ms、v/a 最大间隔 45/78ms、PASS。
- 单元测试：`ctest -R av_start_aligner` 覆盖对齐器的等待/对齐/丢弃/超时/复位逻辑（无设备依赖）。
- 人工：`ffplay source/av_sync_test.flv` 核对唇音。

**经验要点**：

- 任何「帧号 × 名义时长」合成的 PTS 都是定时炸弹：只要丢一帧，时钟就与真实时间永久漂移。采集设备必须用真实时钟（驱动时间戳或单调时钟打点）。
- 编码器必须透传输入帧的时间戳语义，不能自己另起一套计数器时间轴。
- 双路起始对齐的本质是「等齐首包再定零点」；对尚未对齐的流**丢弃**要非常小心——异步网格下会饿死。
- 文件可验证「时间线对齐与无漂移」；感知唇音延迟还包含曝光/缓冲/编码等管线固有延迟，最终要用播放端实测确认。
