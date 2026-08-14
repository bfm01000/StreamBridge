88=9
.# Android 相机采集推流验证流程

本文档用于验证反向链路：

```text
Android Camera2 -> MediaCodec H.264 -> FFmpeg RTMP Publisher -> SRS -> Linux ffplay/ffprobe
```

当前 Android 端推流是 video-only MVP：

- 视频：H.264 baseline
- 分辨率：1280x720
- 帧率：30 fps
- 码率：2 Mbps
- GOP：2 秒
- 音频：暂未接入

## 1. Linux 端启动 SRS

在 Linux 上启动 RTMP 服务，监听 `1935`：

```bash
SRS=~/workspace/tools/srs-centos7/SRS-CentOS7-x86_64-6.0-r0/usr/local/srs/objs/srs
SRS_DIR=$(dirname $(dirname "$SRS"))
cd "$SRS_DIR" && mkdir -p objs

cat > /tmp/srs.conf << 'EOF'
listen 1935;
max_connections 100;
daemon off;
srs_log_tank console;
srs_log_level info;
vhost __defaultVhost__ {}
EOFK

$SRS -c /tmp/srs.conf
```

另开一个 Linux 终端确认端口：

```bash
ss -tlnp | grep 1935
```

预期能看到 `0.0.0.0:1935` 或 `*:1935`，表示手机可以从局域网访问。

## 2. 获取 Linux 局域网 IP

Android 真机和 Linux 必须在同一个局域网，或者 Android 能访问 Linux 的 `1935` 端口。

```bash
ip addr show | grep "inet " | grep -v 127.0.0.1
```

记下 Linux 的局域网 IP，例如：

```text
192.168.31.57
```

后面 Android 推流地址使用：

```text
rtmp://192.168.31.57:1935/live/android_camera
```

## 3. Linux 端先准备观看命令

建议先在 Linux 上开好 `ffplay`，等待 Android 推流进来：

```bash
ffplay -fflags nobuffer -flags low_delay \
  rtmp://127.0.0.1:1935/live/android_camera
```

如果 Linux 没有图形界面，用 `ffprobe` 看流参数：

```bash
ffprobe -v error \
  -show_entries stream=codec_name,width,height,r_frame_rate,avg_frame_rate \
  rtmp://127.0.0.1:1935/live/android_camera
```

也可以录 10 秒文件后再查看：

```bash
timeout 10 ffmpeg -i rtmp://127.0.0.1:1935/live/android_camera \
  -t 10 -c copy /tmp/android_camera.flv

ffprobe -v error -show_streams /tmp/android_camera.flv
```

## 4. Android 端构建和安装

在 Windows 工程目录执行构建：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\android-verify.ps1
```

预期：

```text
BUILD SUCCESSFUL
compileDebugKotlin NO-SOURCE
android/app/build/outputs/apk/debug/app-debug.apk
```

安装到真机：

```powershell
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

启动：

```powershell
adb shell am start -n com.streambridge.android/.MainActivity
```

## 5. Android App 操作步骤

1. 打开 App，进入主页。
2. 点击 `Android采集推流端`。
3. 在 URL 输入框填入 Linux 的局域网地址：

   ```text
   rtmp://192.168.31.57:1935/live/android_camera
   ```

4. 点击 `TCP测试`。
   预期显示：

   ```text
   OK: 192.168.31.57:1935
   ```

5. 点击 `开始推流`。
6. 首次使用会弹出相机权限，选择允许。
7. Android 页面应显示相机预览画面。
8. Linux `ffplay` 窗口应显示 Android 相机画面。

## 6. 预期结果

Linux `ffprobe` 预期能看到：

```text
codec_name=h264
width=1280
height=720
r_frame_rate=30/1
```

Android 页面状态预期类似：

```text
RTMP publisher started
Publish: Publishing packets=...
```

SRS 控制台预期能看到客户端 publish 连接和 `/live/android_camera` 流。

## 7. 推荐验证顺序

优先按这个顺序排查，能最快定位问题：

1. Linux 上 `ss -tlnp | grep 1935` 确认 SRS 已监听。
2. Android 页面点击 `TCP测试`，确认手机能连到 Linux。
3. Android 点击 `开始推流`，确认手机有相机预览。
4. Linux 用 `ffprobe` 确认 RTMP 流存在。
5. Linux 用 `ffplay` 观看画面。
6. 最后再用 Linux 接收端程序验证。

## 8. 常见问题

### TCP 测试失败

可能原因：

- Android 和 Linux 不在同一个局域网。
- URL 里用了 `127.0.0.1`。真机不能用 Linux 的 localhost。
- Linux 防火墙拦截 `1935`。
- SRS 没启动，或者只监听 `127.0.0.1`。

检查：

```bash
ss -tlnp | grep 1935
hostname -I
```

### Android 有预览，但 Linux 看不到流

可能原因：

- RTMP URL 路径和 Linux 观看路径不一致。
- SRS 拒绝 publish。
- native publisher 写 header 或 packet 失败。

Android 侧看状态：

```text
PublishError ...
```

Linux 侧看 SRS 控制台日志中是否出现 `/live/android_camera`。

### Linux ffplay 黑屏或等待

先用 `ffprobe` 验证是否有流：

```bash
ffprobe -v error -show_streams \
  rtmp://127.0.0.1:1935/live/android_camera
```

如果 `ffprobe` 没输出，说明流没推上来；如果有 H.264 参数，问题多半在 `ffplay` 播放环境。

### Android 没有相机画面

检查：

- 是否授权 Camera 权限。
- 是否进入的是 `Android采集推流端` 页面。
- 是否点击了 `开始推流`。
- 是否有其他 App 占用相机。

### 第二次推流失败

当前代码已经在 `FFmpegRTMPPublisher::open()` 中 reset stop token，正常应支持停止后再次开始。

如果仍失败，建议换一个流名验证：

```text
rtmp://192.168.31.57:1935/live/android_camera2
```

## 9. 验收标准

Android 采集推流端可认为验证通过，需要满足：

- Android 页面能显示实时相机预览。
- `TCP测试` 连接 Linux `1935` 成功。
- 点击 `开始推流` 后状态进入 `RTMP publisher started` 或 `Publishing packets=...`。
- Linux `ffprobe` 能看到 H.264 1280x720 30fps。
- Linux `ffplay` 能看到 Android 相机画面。
- 停止推流后再次开始，仍能重新推流。

## 10. 当前限制

- 目前只推视频，不推音频。
- 推流参数固定在代码中，暂未做 UI 配置。
- Android 端使用 Camera2 + MediaCodec Java API，native 层负责 RTMP/FLV 写入。
- 端到端延迟还没有精确统计；后续需要在 Android 采集时间戳和 Linux 接收显示时间之间建立统一测量方案。
