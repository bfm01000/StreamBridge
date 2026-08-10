# StreamBridge Decoder Architecture v2.1

（v2 评审后修订，7 处关键修正。）

## 1. 问题回顾

| v1 | v2 | v2.1 |
|----|----|------|
| `output_index` 泄漏到 Session | 用 `frame_id` 隐藏 ✅ | 保留 ✅ |
| Session 判断 `output_mode()` | 去掉分支 ✅ | 保留 ✅ |
| `has_output=false` | `Result::Timeout` | `DecodeStatus::TryAgain` |
| `release_frame(id, bool render)` | 保留 MediaCodec 风格 ❌ | `present_frame` / `discard_frame` |
| `VideoFrameHandle` 万能 struct | 所有字段共存 ❌ | `std::variant<CpuHandle, SurfaceHandle, ...>` |
| `void* surface` 暴露 Surface | 泄漏 ANativeWindow ❌ | `DecoderSurfaceHandle`（opaque） |
| FFmpeg PtsFifo 作为主 PTS | 没改 | 优先 `best_effort_timestamp` |
| FFmpeg `frame_map_` | 不必要 | 删除，shared_ptr 管理生命周期 |

---

## 2. 核心接口（v2.1 定稿）

```cpp
// ── common/include/streambridge/codec.h ──

// 解码状态（替代 has_output + Timeout）
enum class DecodeStatus {
    FrameReady,   // receive_frame 拿到一帧
    TryAgain,     // 暂无输出，需要更多输入（EAGAIN / timeout）
    EndOfStream,  // 流结束
};

// CPU 帧 handle
struct CpuFrameHandle {
    std::shared_ptr<CpuFrame> frame;  // owning
};

// MediaCodec / VAAPI Surface 输出（opaque）
struct DecoderSurfaceHandle {
    // Session 不需要知道 ANativeWindow*
    // 内部 decoder 已知输出目标
};

// 未来扩展
struct DmaBufFrameHandle { /* fd + planes */ };
struct GpuTextureHandle { /* texture id */ };

// 标记联合 — 编译期保证只有一个分支有效
using FramePayload = std::variant<
    CpuFrameHandle,
    DecoderSurfaceHandle,
    DmaBufFrameHandle,
    GpuTextureHandle
>;

struct DecodeOutput {
    uint64_t frame_id = 0;
    int64_t  pts_us = 0;
    FramePayload payload;
};

// 解码器能力
struct DecoderCapability {
    bool hardware = false;
    bool supports_surface_output = false;
    bool supports_cpu_output = false;
};

// ── 统一接口 ──
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual Result<void> open(const StreamInfo& info) = 0;
    virtual Result<DecodeStatus> send_packet(const MediaPacket& packet) = 0;
    virtual Result<DecodeOutput> receive_frame(int timeout_ms) = 0;

    // 向显示目标提交帧（DecoderSurface / DMA-BUF / Texture 用）
    virtual Result<void> present_frame(uint64_t frame_id, int64_t target_time_ns) = 0;

    // 丢弃帧（所有模式通用）
    virtual Result<void> discard_frame(uint64_t frame_id) = 0;

    virtual void flush() = 0;
    virtual void close() = 0;
    virtual DecoderCapability capability() const = 0;
};
```

---

## 3. 实现细节

### 3.1 FFmpegDecoder

```cpp
// open: avcodec_open2

send_packet:
    avcodec_send_packet

receive_frame:
    ret = avcodec_receive_frame → AVFrame*
    if (ret == EAGAIN) return DecodeStatus::TryAgain
    if (ret == EOF)    return DecodeStatus::EndOfStream

    // PTS: 优先 best_effort_timestamp → frame->pts → fallback PtsFifo
    int64_t pts = av_frame_get_best_effort_timestamp(frame);
    pts_us = av_rescale_q(pts, codec_tb, {1, 1'000'000});

    // CPU frame
    cpu = make_shared<CpuFrame>(swscale(frame));

    return DecodeOutput{
        frame_id = next_id_++,
        pts_us,
        payload = CpuFrameHandle{cpu}
    };

present_frame:  // CPU 模式不适用 — 由 Renderer 负责
    return err("not supported for CPU output");

discard_frame:
    // shared_ptr 析构自动释放，无需额外操作
    return ok();
```

**关键变化**：不再需要 `frame_map_`——`shared_ptr<CpuFrame>` 直接放在 `DecodeOutput` 里，Session 或 Renderer 持有最后一个引用时自动释放。

### 3.2 MediaCodecDecoder

```cpp
// open: AMediaCodec_configure(codec, format, surface, ...)

send_packet:
    idx = dequeueInputBuffer → memcpy → queueInputBuffer(idx, pts)

receive_frame:
    idx = dequeueOutputBuffer(timeout_ms * 1000)
    if (idx == TRY_AGAIN_LATER)  return DecodeStatus::TryAgain
    if (idx == FORMAT_CHANGED)   { update format; continue }
    if (idx == EOF)              return DecodeStatus::EndOfStream

    frame_id = next_id_++;
    frame_map_[frame_id] = idx;   // ★ MediaCodec 专用：frame_id → buffer index

    return DecodeOutput{
        frame_id,
        pts_us = buf_info.presentationTimeUs,
        payload = DecoderSurfaceHandle{}
    };

present_frame(frame_id, target_time_ns):
    idx = frame_map_[frame_id];
    AMediaCodec_releaseOutputBuffer(codec, idx, true);
    // 可选：AMediaCodec_releaseOutputBufferAtTime(codec, idx, target_time_ns);
    frame_map_.erase(frame_id);

discard_frame(frame_id):
    idx = frame_map_[frame_id];
    AMediaCodec_releaseOutputBuffer(codec, idx, false);
    frame_map_.erase(frame_id);
```

**关键变化**：`frame_map_` 只在 MediaCodec 内部使用——`frame_id → output buffer index` 的映射仅此一处需要。

---

## 4. 所有权模型

| 资源类型 | 所有权 | 释放方式 |
|---------|--------|---------|
| `CpuFrameHandle` | `shared_ptr` owning | 最后一个引用析构 |
| `DecoderSurfaceHandle` | Decoder owning, Session leasing | `present_frame` 或 `discard_frame` |
| `DmaBufFrameHandle` | RAII handle（dup'd fd） | handle 析构时 close(fd) |
| `GpuTextureHandle` | Decoder owning, lease | `discard_frame` 通知归还 |

核心语义：
> **`DecodeOutput` 持有底层资源的有效 lease。在 `present_frame` / `discard_frame` 被调用之前，资源必须保持有效。**

---

## 5. Session 层

```cpp
// native_playback_session.cpp
video_loop() {
    while (pop packet) {
        decoder_->send_packet(packet);

        while (true) {
            auto result = decoder_->receive_frame(200);
            if (result.status == TryAgain) break;
            if (result.status == EndOfStream) { drain; return; }

            auto& out = *result;

            // AV sync: 基于 out.pts_us 决策
            Decision d = sync_controller_.decide(TimePointUs{out.pts_us}, clock_.now());

            if (d == Drop) {
                decoder_->discard_frame(out.frame_id);
                continue;
            }

            if (d == Wait) {
                sleep(d.wait_us);
                d = re_decide(...);
                if (d == Drop) { decoder_->discard_frame(out.frame_id); continue; }
            }

            // ── 渲染（根据 payload 类型分发）──
            std::visit(overloaded{
                [&](CpuFrameHandle& cpu) {
                    renderer_.render(cpu.frame);
                    decoder_->discard_frame(out.frame_id);
                },
                [&](DecoderSurfaceHandle&) {
                    decoder_->present_frame(out.frame_id, d.target_time_ns);
                },
                [&](auto&) {
                    // future: DMA-BUF / GPU Texture
                }
            }, out.payload);
        }
    }
}
```

`std::visit` + `overloaded` 模板替代 if-else 类型判断。

---

## 6. PTS 策略

| 优先级 | 来源 | 场景 |
|--------|------|------|
| 1 | `av_frame_get_best_effort_timestamp(frame)` | FFmpeg ≥ 3.0 |
| 2 | `frame->pts`（time_base 转换） | FFmpeg 直通 |
| 3 | `PtsFifo.pop()` | fallback：H.264 B 帧重排后仍无法获取 PTS |

MediaCodec 直接使用 `AMediaCodecBufferInfo::presentationTimeUs`。

---

## 7. 修改文件列表

| 文件 | 操作 |
|------|------|
| `common/include/streambridge/codec.h` | 重写 IVideoDecoder、DecodeOutput、FramePayload、DecoderCapability |
| `common/src/ffmpeg/ffmpeg_video_decoder.h` | 重写，实现新接口 |
| `common/src/ffmpeg/ffmpeg_video_decoder.cpp` | 重写，`best_effort_timestamp` PTS、去掉 `frame_map_` |
| `android/.../mediacodec/mediacodec_video_decoder.h` | 重写，`frame_map_` + `present_frame`/`discard_frame` |
| `android/.../mediacodec/mediacodec_video_decoder.cpp` | 重写 |
| `android/.../playback/native_playback_session.cpp` | 用 `std::visit` 替代 `output_mode()` 分支 |
| `android/.../playback/native_video_renderer.cpp` | `render()` 接受 `CpuFrameHandle` |
| `common/include/streambridge/pts_fifo.h` | 降级为 fallback（保留但降低优先级） |
