# 推流测试指南

本文档提供 StreamBridge 推流端的快速测试步骤。**你需要先选择测试目标**，然后按对应路径操作。

测试素材：`/home/bfm01000/下载/FRXXZ.mp4`（852×480 H.264 + AAC，推流时自动缩放到 1280×720）。

---

## 0. 选择测试路径

```text
                    ┌─── 路径 A: 本地验证 ─── 在 Linux 上用 ffplay 观看
                    │    (需要 Linux 有图形界面)
启动 SRS + 推流 ───┤
                    │
                    └─── 路径 B: Android 验证 ─── Android 手机/模拟器拉流播放
                         (手机和 Linux 需在同一局域网)
```

---

## 路径 A：本地验证（ffplay 观看）

### A1. 启动 SRS

```bash
SRS=~/workspace/tools/srs-centos7/SRS-CentOS7-x86_64-6.0-r0/usr/local/srs/objs/srs
SRS_DIR=$(dirname $(dirname $SRS))
cd "$SRS_DIR" && mkdir -p objs

cat > /tmp/srs.conf << 'EOF'
listen 1935;
max_connections 100;
daemon off;
srs_log_tank console;
srs_log_level info;
vhost __defaultVhost__ {}
EOF

# 如果 SRS 已在运行，跳过此步
nohup $SRS -c /tmp/srs.conf > /tmp/srs.log 2>&1 &

# 确认端口监听
ss -tlnp | grep 1935
```

### A2. 启动推流

```bash
cd ~/workspace/StreamBridge

# 本地推流（loop 默认开启，无需 --loop）
./linux/build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/test \
  --video-source '/home/bfm01000/下载/FRXXZ.mp4' \
  --enable-audio \
  --audio-source '/home/bfm01000/下载/FRXXZ.mp4' \
  --audio-backend file \
  --log-level info

# 如果手机和 Linux 在同一局域网，用 LAN IP 推流：
# ./linux/build/app/streambridge_publisher \
#   --rtmp-url rtmp://192.168.31.57:1935/live/test \
#   --video-source '/home/bfm01000/下载/FRXXZ.mp4' \
#   --enable-audio \
#   --audio-source '/home/bfm01000/下载/FRXXZ.mp4' \
#   --audio-backend file \
#   --log-level info


```

看到 `state Running` 表示推流成功。

### A3. 观看（另开终端）

```bash
# 方式 1: 用 ffplay 实时播放（需要图形界面）
ffplay rtmp://127.0.0.1:1935/live/test

# 方式 2: 无图形界面时，拉流保存为文件，再传到有 GUI 的机器播放 rtmp://192.168.31.57:1935/live/test
timeout 10 ffmpeg -i rtmp://127.0.0.1:1935/live/test \
  -t 10 -c copy /tmp/capture.flv
# 把 /tmp/capture.flv 传到有播放器的机器，用任意播放器打开

# 方式 3: 用 ffprobe 只看参数（不需要 GUI）
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels \
  rtmp://127.0.0.1:1935/live/test
# 预期: codec_name=h264 width=1280 height=720
#       codec_name=aac sample_rate=48000 channels=2
```

### A4. 停止

```bash
# Ctrl+C 停止推流程序
# 如需停止 SRS:
pkill -f "objs/srs"
```

---

## 路径 B：Android 远程验证（手机/模拟器）

### B1. 确认网络拓扑

```text
Linux (推流端) ──RTMP──> SRS (Linux 上 1935 端口) <──RTMP── Android (播放端)
```

Android 需要能访问 Linux 的 1935 端口。

### B2. 获取 Linux 局域网 IP

```bash
ip addr show | grep "inet " | grep -v 127.0.0.1
# 示例输出: inet 192.168.1.100/24 ...
#                ^^^^^^^^^^^^ 记住这个 IP
```

### B3. 启动 SRS + 推流（同路径 A1+A2）

```bash
# 1) 启动 SRS（同 A1）
# 2) 启动推流（同 A2，流名可用 /live/mobile）
cd ~/workspace/StreamBridge

./linux/build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/mobile \
  --video-source '/home/bfm01000/下载/FRXXZ.mp4' \
  --enable-audio \
  --audio-source '/home/bfm01000/下载/FRXXZ.mp4' \
  --audio-backend file \
  --log-level info
```

### B4. 验证 Linux 端口对 Android 可达

在 Android 设备浏览器中访问（或在 Linux 上测试端口开放）：

```bash
# 检查 SRS 是否监听全网（0.0.0.0 而非 127.0.0.1）
ss -tlnp | grep 1935
# 预期: LISTEN 0 512 0.0.0.0:1935 ...
#                     ^^^^^^^^^ 必须是 0.0.0.0 或 *，不能是 127.0.0.1
```

### B5. Android 端操作

**真机：**

App 输入 RTMP URL（替换为你的 Linux IP）：
```text
rtmp://192.168.1.100:1935/live/mobile
```
点击 **Start** 开始播放。

**Android 模拟器（运行在 Linux 上）：**

模拟器中 `10.0.2.2` 映射到宿主机 localhost：
```text
rtmp://10.0.2.2:1935/live/mobile
```

### B6. 先验证 SRS 流正常（排查用）

如果 Android 端放不了，先在 Linux 上确认流没问题：

```bash
ffprobe -v error -show_entries stream=codec_name,width,height \
  rtmp://127.0.0.1:1935/live/mobile
# 有输出 = 流正常，问题在 Android 网络/APP 端
# 无输出 = 推流有问题，回去检查 A2
```

---

## 常见问题

| 现象 | 原因 | 解决 |
|---|---|---|
| `Connection refused` | SRS 未启动 | `ss -tlnp \| grep 1935` 确认 |
| `stream is busy` | 上次推流残留 | `pkill -f streambridge` 后换流名重试 |
| ffplay 报 `Connection refused` | SRS 未启动或流名不对 | 确认 A1 SRS 已启，A2 推流中 |
| 保存的 FLV 文件只有几 KB | 拉流前推流未就绪 | 等 A2 输出 `Running` 后再拉 |
| Android 连不上 | 网络不通或 IP 不对 | Linux 和手机互 ping；检查防火墙 |
| Android 连上但黑屏 | APP 播放后端不支持 RTMP | 先用 HTTP MP4 URL 验证 APP；RTMP 需 FFmpeg 后端 |
| 模拟器内连接失败 | 用了 127.0.0.1 | 改用 `10.0.2.2` |
| 推流中 fps 太低 | CPU 编码 1280×720 吃力 | 加 `--video-bitrate 500000` 降码率 |

---

## 参数速查

| 参数 | 说明 | 默认值 |
|---|---|---|
| `--rtmp-url` | RTMP 推流地址 | 必填 |
| `--video-source` | 视频源（文件路径 / lavfi filter / V4L2 设备） | 必填 |
| `--enable-audio` | 启用音频 | false |
| `--audio-source` | 音频源 | 同视频源 |
| `--audio-backend` | `file` / `lavfi` / `alsa` | file |
| `--loop` | 文件循环播放（模拟直播） | true（默认开启） |
| `--no-loop` | 播完一次后停止 | — |
| `--no-throttle` | 不限速全速推流（基准测试） | false |
| `--video-bitrate` | 视频码率 bps | 2000000 |
| `--video-width` / `--video-height` | 输出分辨率 | 1280 / 720 |
| `--log-level` | `debug` / `info` / `warn` / `error` | info |

---

## 日志解读

正常：
```text
[I] [session] state Idle -> Preparing -> Prepared -> Running
[I] [main] uptime=10s cap=... enc=... sent=... drop=0 ...
[I] [main] exiting
```

异常信号：
- `drop > 0`：有丢帧
- `state ... -> Error`：启动/运行中出错
- `encode error`：编码失败
- `stream is busy`：流名被占用

---

## 音画同步验证（camera + ALSA 真实设备）

`av_sync_capture_test` 走完整生产链路（V4L2/ALSA 采集 → H.264/AAC 编码 → FLV mux），
默认录制 10 秒到 `source/av_sync_test.flv`（相对运行目录），再离线分析音视频时间线。

### 运行方式

```bash
cd linux/build
make av_sync_capture_test

./tests/av_sync_capture_test                  # 录制 10s 并分析（默认）
./tests/av_sync_capture_test --duration 30    # 自定义时长
./tests/av_sync_capture_test --output x.flv   # 自定义输出路径
./tests/av_sync_capture_test --analyze x.flv  # 只分析已有文件
ctest -R av_sync                              # ctest 入口（同默认 10s）
```

设备缺失（无摄像头/声卡）时打印 SKIP 并以退出码 0 结束。

### 判定阈值（录制 10s）

| 指标 | 阈值 | 含义 |
|---|---|---|
| start skew（音/视首包差） | ±100 ms | `AvStartAligner` 保证两路从同一零点开始 |
| drift（视频跨度-音频跨度） | ±100 ms | 两路共用单调时钟，时间线应平行 |
| 视频最大帧间隔 | ≤200 ms | 无长时间丢帧空洞 |
| 音频最大包间隔 | ≤150 ms | 无 XRUN/队列丢包空洞 |
| 单调性 | 必须 | 时间线不允许回退 |

预期输出：

```text
=== AV sync analysis ===
video frames : 291
audio packets: 450
duration     : 9642 ms
start skew   : +20.0 ms
drift        : +9.0 ms
v max gap    : 45.0 ms
a max gap    : 78.0 ms
monotonic    : yes
verdict      : PASS
```

### 时间戳架构（2026-08-14 修复后）

- **采集端**：V4L2 优先用驱动 `buf.timestamp`（uvcvideo 帧完成时刻，CLOCK_MONOTONIC），
  驱动未提供时回退到出队时刻；ALSA 用 `readi` 完成时刻。两路共用 CLOCK_MONOTONIC 时间域。
  禁止 `frame_idx × 名义时长` 合成 PTS——丢帧/XRUN 后合成时钟与真实时间漂移。
- **音频编码**：AAC 帧 PTS = 累积器首采样采集时刻 + 本轮帧偏移 × 帧长，
  换算到输出采样时钟。累积器每轮重启时帧偏移归零（不能用全局帧计数，会重复叠加）。
- **mux 对齐**：`AvStartAligner`（common）以较晚启动的一路首包为零点，
  丢弃另一路早于零点的包；对齐前只等待不丢弃（过早丢弃会让两路首包永远
  不同时在队，对齐饿死）。纯逻辑单元测试见 `ctest -R av_start_aligner`。

### 排查指引

- `start skew` 大：对齐器未生效（检查 `AV start aligned` 日志的 base 值）
- `drift` 大：某一路 PTS 不是单调时钟（检查采集端时间戳来源）
- `a max gap` 周期性 ~350ms：音频编码器累积轮次 PTS 叠加（`acc_frame_idx_` 未归零）
- 录制文件 `source/av_sync_test.flv` 可用 `ffplay` 人工核对唇音

注意：文件可验证「时间线对齐与无漂移」；感知唇音延迟还包含采集管线固有延迟
（曝光、缓冲、编码），需在播放端用节拍器/嘴型做最终验证。

---

## 播放端验证（Linux 拉流播放）

反向链路（Android 推流 → Linux 播放）的播放端用法、指标与排查详见
`docs/build-and-run.md` §9。快速自检命令：

```bash
# 终端 1：模拟对端推流（SRS 需已运行）
~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffmpeg -re \
  -i /home/bfm01000/下载/FRXXZ.mp4 \
  -c:v libx264 -preset ultrafast -tune zerolatency -vf scale=640:480 \
  -c:a aac -b:a 128k -f flv rtmp://127.0.0.1:1935/live/simulated

# 终端 2：Linux 播放端（窗口播放，ESC 退出；或 --duration 20 自动退出）
./linux/build/player/streambridge_player \
  --url rtmp://127.0.0.1:1935/live/simulated --duration 20
```

验收要点：`rendered` 增速≈源帧率、`av_diff_us` 收敛 ±50ms、`dropped=0`、
`reconnects=0`；有音频源时 `audio_frames` 匀速增长（音频主时钟生效）。
