# Timestamp And A/V Sync

本文档定义 StreamBridge 的时间戳、time base、音视频同步和可观测策略。第一阶段目标是 Linux 推流到 Android 播放，但设计应能复用于未来 Android 推流到 Linux。

## 1. Principles

- 公共层内部统一使用微秒，变量名必须带 `_us`。
- 靠近 FFmpeg、编码器、FLV/RTMP 或音频设备边界时允许使用各自 time base，但进入 common 队列前必须转换。
- 采集时间戳、编码 PTS/DTS、封装时间戳、解码帧 PTS、音频播放时钟必须可追踪。
- 播放端默认使用音频实际播放进度作为主时钟，视频同步到音频。
- “提交给音频设备”不等于“已经播放到扬声器”。

## 2. Time Domains

| 时间域 | 单位 | 来源 | 使用位置 |
| --- | --- | --- | --- |
| monotonic_us | us | `CLOCK_MONOTONIC` / Android `clock_gettime` | 公共内部时间轴、日志 |
| capture_video_us | us | V4L2/Camera timestamp 或采集回调时间 | 原始视频帧 |
| capture_audio_us | us | ALSA/AudioRecord timestamp 或回调估算 | 原始音频帧 |
| codec_pts | encoder time base | 编码器输入输出 | FFmpeg/MediaCodec 边界 |
| avpacket_pts/dts | stream time base | FFmpeg packet | mux/demux |
| rtmp_ts_ms | ms | FLV/RTMP timestamp | 网络封装 |
| decoded_frame_pts_us | us | 解码器输出恢复 | 播放队列 |
| audio_played_us | us | 音频设备实际播放位置 | 播放主时钟 |

## 3. Base Conversion Formulas

通用转换：

```text
seconds = value * time_base.num / time_base.den
us = round(seconds * 1000000)
value = round(us * time_base.den / (time_base.num * 1000000))
```

FFmpeg `AVRational tb`：

```text
pts_us = av_rescale_q(pts, tb, {1, 1000000})
pts_in_tb = av_rescale_q(pts_us, {1, 1000000}, tb)
```

FLV/RTMP：

```text
rtmp_ts_ms = max(0, round((pts_us - stream_start_us) / 1000))
pts_us_from_rtmp = stream_start_us + rtmp_ts_ms * 1000
```

首帧归一化：

```text
stream_start_us = min(first_audio_pts_us, first_video_pts_us)
normalized_pts_us = raw_pts_us - stream_start_us
```

如果首个流暂时缺失，允许用先到流初始化 `stream_start_us`，但当另一路到达时必须记录偏移并检测异常。

## 4. Linux Capture Timestamps

### Video

优先顺序：

1. V4L2 buffer timestamp，确认是否为 monotonic。
2. FFmpeg avdevice packet PTS 映射后的时间。
3. 采集线程收到帧时的 `CLOCK_MONOTONIC`，作为降级方案。

要求：

- 记录 `video_capture_source`。
- 若设备时间戳不单调，回退到 monotonic receive time 并记录 warning。
- 视频帧进入 `RawVideoQueue` 前必须有 `pts_us`。

### Audio

优先顺序：

1. ALSA 硬件时间戳或 frame position。
2. FFmpeg avdevice packet PTS。
3. 音频回调时间减去 buffer duration 的估算值。

估算公式：

```text
frame_duration_us = nb_samples * 1000000 / sample_rate
capture_audio_us = callback_time_us - queued_or_device_delay_us
```

如果无法准确获取硬件时间戳，必须在日志中标记 `audio_timestamp_estimated=true`。

## 5. Android Capture Timestamps For Future Direction

未来 Android 推流时：

- Camera2 可使用 `SENSOR_TIMESTAMP`，通常为纳秒级单调时间。
- AudioRecord 需优先使用 `getTimestamp` 或按 read position 估算。
- MediaCodec 输入 PTS 使用采集归一化后的 `pts_us`。

Android Camera 与 AudioRecord 可能来自不同硬件时钟，进入 common 前必须映射到同一 monotonic_us 轴。

## 6. Encode And Mux

编码器输入：

```text
encoder_input_pts = convert_us_to_codec_tb(frame.pts_us)
```

编码器输出：

```text
packet.pts_us = convert_codec_pts_to_us(encoded_pts)
packet.dts_us = convert_codec_pts_to_us(encoded_dts)
```

禁用 B 帧后，通常 `dts_us <= pts_us` 且差值较小，很多情况下相等。仍然不要假定一定相等，日志保留两者。

RTMP 发布：

- 视频关键帧前必须已经发送 H.264 sequence header。
- AAC 音频数据前必须已经发送 AudioSpecificConfig。
- RTMP timestamp 使用归一化毫秒。
- 发生重连时，新连接重新发送 sequence headers，并从新的首个 packet 重新归一化。

## 7. Demux And Decode

Android 拉流后：

```text
demux_packet_pts_us = convert_stream_tb_to_us(packet.pts)
normalized_pts_us = demux_packet_pts_us - first_demux_pts_us
decoded_frame_pts_us = best_effort_timestamp_us or packet_pts_us
```

若 FFmpeg decoder 输出 `best_effort_timestamp`，优先使用它。若缺失，使用输入 packet PTS 传播值，并记录 `pts_recovered_from_packet=true`。

解码后的 `VideoFrame` 和 `AudioFrame` 必须携带 `pts_us`。无 PTS 的帧不能直接进入同步队列，必须丢弃或按连续 sample/frame duration 估算并记录。

## 8. Audio Master Clock

音频主时钟定义：

```text
audio_clock_us = first_audio_pts_us + played_samples * 1000000 / sample_rate
```

其中 `played_samples` 必须尽量表示实际播放到设备的 sample 数，而不是提交到队列的 sample 数。

Android AAudio：

- 优先使用 timestamp 或 frame position 获取硬件播放位置。
- 记录 `submitted_frames`、`played_frames`、`device_latency_us`。
- 若只能获得近似值，使用提交位置减去估算延迟，并标记估算。

无音频时：

- 使用 monotonic wall clock 作为临时主时钟。
- `master_clock_us = first_video_pts_us + (now_us - playback_start_monotonic_us)`。
- 一旦音频恢复，平滑切回音频主时钟，避免画面跳变。

## 9. Video Sync Decisions

定义：

```text
av_diff_us = video_pts_us - audio_clock_us
```

建议阈值：

| 条件 | 决策 |
| --- | --- |
| `av_diff_us > +40000` | 视频太早，等待最多到目标时间；等待期间响应 stop/flush。 |
| `-40000 <= av_diff_us <= +40000` | 立即渲染。 |
| `-120000 <= av_diff_us < -40000` | 视频轻微落后，立即渲染并记录 late。 |
| `av_diff_us < -120000` | 视频严重落后，丢帧直到接近主时钟或遇到关键恢复点。 |

阈值需在真机测试中调整。720p30 下 40 ms 约一帧多一点，120 ms 约 3-4 帧。

## 10. Queue And Rebuffer Rules

播放端启动：

1. 网络线程读取 header。
2. 至少获得音频和视频基础信息。
3. 缓冲到音频 100-300 ms 或视频若干帧。
4. 初始化 `playback_start_monotonic_us`。
5. 启动音频输出，视频同步到音频。

队列清空：

- 网络重连：abort demux read，flush packet/frame queues，重置 stream start。
- Surface 重建：不清空音频队列；暂停视频渲染，Surface 恢复后按当前 audio clock 丢掉过期视频帧。
- 用户 stop：abort 所有队列和阻塞调用，释放资源。
- 解码错误：按流隔离处理，超过阈值进入 Error 或 Reconnecting。

## 11. Drift Detection

采集端漂移检测：

```text
audio_elapsed_us = audio_samples_captured * 1000000 / sample_rate
video_elapsed_us = last_video_pts_us - first_video_pts_us
drift_us = audio_elapsed_us - video_elapsed_us
```

播放端漂移检测：

```text
observed_av_diff_us = video_pts_us - audio_clock_us
rolling_avg_diff_us = moving_average(observed_av_diff_us, 5s)
```

处理策略：

- 小漂移：视频同步策略自然吸收。
- 中等音频漂移：可在未来用轻微重采样补偿，第一版只记录并告警。
- 大漂移：触发 rebuffer 或重连。

建议告警：

- `abs(rolling_avg_diff_us) > 80000` 持续 5 秒。
- 队列持续增长超过 2 秒。
- 音频 underrun 或 overrun。

## 12. Blocking FFmpeg Interrupt

所有阻塞 FFmpeg 网络读写必须绑定可中断标志：

```text
interrupt_callback returns true when stop_requested or reconnect_requested
```

停止顺序：

1. 设置 stop flag。
2. 调用 FFmpeg interrupt callback 生效。
3. abort 队列。
4. join 网络和解码线程。
5. 关闭 codec/context。

禁止依赖析构函数被动等待网络超时。

## 13. Logging

推荐日志事件：

- `capture.video.frame`
- `capture.audio.frame`
- `encode.input`
- `encode.output`
- `rtmp.publish.packet`
- `rtmp.subscribe.packet`
- `decode.output`
- `audio.submit`
- `audio.played`
- `sync.render`
- `sync.drop`
- `queue.watermark`
- `session.state`
- `session.reconnect`

关键字段：

```text
ts_us module session_id thread event pts_us dts_us tb queue_size queue_ms audio_clock_us video_pts_us av_diff_us latency_us err
```

## 14. Tests

单元测试：

- `av_rescale` 等价转换。
- 首帧归一化。
- RTMP ms 与 us 往返误差。
- 无 PTS 恢复策略。
- Sync decision 阈值。
- Queue abort/flush/timeout。

集成测试：

- 本地文件推 RTMP 到 SRS，再用 ffplay 验证。
- Linux 推流日志中的 capture/encode/rtmp 时间戳连续。
- Android 拉流日志中的 demux/decode/audio/video 时间戳连续。
- 30 分钟运行后 `av_diff_us`、队列水位和内存没有持续恶化。

## 15. Open Questions

需要用户或后续实测确认：

1. Android 音频输出 API 选 AAudio、AudioTrack 还是 Oboe。
2. Android 真机最低 API 和可用设备。
3. Linux 运行环境是否能访问真实摄像头和麦克风。
4. SRS 是本地、Docker 还是远程服务。
5. 第一版是否接受音频时间戳估算，还是必须使用硬件 timestamp。
