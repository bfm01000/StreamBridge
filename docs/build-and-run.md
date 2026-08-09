# 构建与运行指南

本文档记录 Linux 端开发环境的搭建、构建、运行和**独立验证**步骤。每个里程碑都附带用户可自行执行的验证命令。

---

## 1. 环境概览

| 项目 | 值 |
|---|---|
| OS | Ubuntu 24.04 x86_64 |
| 编译器 | gcc/g++ 13.3.0 (C++17) |
| 构建 | CMake 3.28.3 |
| 代理 | Clash (verge-mihomo), socks5h://127.0.0.1:7897 |

---

## 2. 工具安装

### 2.1 静态 FFmpeg（含 libx264，开箱即用）

```bash
mkdir -p ~/workspace/tools && cd ~/workspace/tools

curl -x socks5h://127.0.0.1:7897 -L \
  "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz" \
  -o ffmpeg-static.tar.xz

tar xf ffmpeg-static.tar.xz
# 产物: ffmpeg-7.0.2-amd64-static/ffmpeg, ffprobe
```

**验证工具已安装**：
```bash
~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffmpeg -version 2>&1 | head -1
# 预期: ffmpeg version 7.0.2-static ...

~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffmpeg -encoders 2>/dev/null | grep libx264
# 预期:  V....D libx264  libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10
```

### 2.2 SRS RTMP 服务器

```bash
cd ~/workspace/tools

curl -x socks5h://127.0.0.1:7897 -L \
  "https://github.com/ossrs/srs/releases/download/v6.0-r0/SRS-CentOS7-x86_64-6.0-r0.zip" \
  -o srs-centos7.zip

unzip -o srs-centos7.zip -d srs-centos7
```

**验证工具已安装**：
```bash
SRS=~/workspace/tools/srs-centos7/SRS-CentOS7-x86_64-6.0-r0/usr/local/srs/objs/srs
$SRS --help 2>&1 | head -3
# 预期: SRS/6.0.184(Hang), https://github.com/ossrs/srs, MIT
```

---

## 3. Milestone 1 验证：RTMP 链路

> **验证目标**：确认 FFmpeg libx264 编码 → RTMP 推流 → SRS 接收 → 拉流验证 全链路可用。
> **不需要编译任何 C++ 代码**。

### 3.1 启动 SRS

```bash
# 终端 1
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

$SRS -c /tmp/srs.conf
```

**验证 SRS 已启动**（另开终端）：
```bash
ss -tlnp | grep 1935
# 预期: LISTEN 0 512 0.0.0.0:1935 ...
```

### 3.2 推流（终端 2）

```bash
FFMPEG=~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffmpeg

$FFMPEG -re \
  -f lavfi -i "testsrc=size=1280x720:rate=30:duration=60" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=60" \
  -c:v libx264 -preset ultrafast -tune zerolatency \
  -x264-params "keyint=60:no-b-adapt=1:bframes=0" \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://127.0.0.1:1935/live/test
```

**验证推流成功**：
- 终端输出持续刷帧：`frame=  ... fps=... size=... time=...`
- 出现 `[libx264 @ ...]` 编码器信息
- 最终显示 `video:...KiB audio:...KiB`
- 不应出现 `Connection refused` 或 `Connection timed out`

### 3.3 拉流验证（终端 3）

```bash
# 检查流参数
FFPROBE=~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffprobe

$FFPROBE -v quiet -show_streams rtmp://127.0.0.1:1935/live/test 2>&1 | \
  grep -E "codec_name|width|height|sample_rate|channels"

# 预期输出（顺序可能不同）:
# codec_name=aac
# sample_rate=48000
# channels=2
# codec_name=h264
# width=1280
# height=720
```

**验证推流—拉流端到端**：
```bash
# 用 ffplay 观看（需图形界面）
ffplay rtmp://127.0.0.1:1935/live/test
# 预期: 看到彩色条纹测试画面 + 听到 440Hz 纯音

# 无图形界面时，用拉流验证不丢帧:
FFMPEG=~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffmpeg
timeout 15 $FFMPEG -v quiet -i rtmp://127.0.0.1:1935/live/test \
  -t 10 -f null - 2>&1
# 预期: 正常输出无错误，exit code 0
```

### 3.4 停止

```bash
# 停止 SRS
pkill -f "objs/srs"
```

### 3.5 M1 验收清单

| 检查项 | 通过标准 |
|---|---|
| SRS 启动 | 监听 1935 端口 |
| FFmpeg 推流 | libx264 编码正常，帧率稳定 30fps |
| ffprobe 检查 | codec=h264 1280x720 + aac 48000Hz |
| 端到端拉流 | 无网络错误，不丢帧 |
| 30 秒稳定性 | 900 帧推流完成，exit code 0 |

---

## 4. Milestone 2 验证：Linux C++ 视频采集推流

> **目标**：编译 `streambridge_publisher` C++ 程序，使用 lavfi 测试源推送 H.264 视频流到 SRS，ffprobe 拉流验证。

### 4.1 构建

```bash
cd linux/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 4.2 启动 SRS

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

nohup $SRS -c /tmp/srs.conf > /tmp/srs.log 2>&1 &
```

### 4.3 推流（视频-only）

```bash
./build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/test \
  --video-source "testsrc=size=1280x720:rate=30" \
  --log-level info
```

### 4.4 验证

```bash
ffprobe -v quiet -show_streams rtmp://127.0.0.1:1935/live/test | \
  grep -E "codec_name|width|height|r_frame_rate"
# 预期: codec_name=h264, width=1280, height=720, r_frame_rate=30/1
```

### 4.5 M2 验收

| 检查项 | 通过标准 |
|---|---|
| 编译 | 零警告 |
| 推流 | 30fps 稳定，zero drops |
| ffprobe | H.264 1280x720 30fps |
| 状态机 | Idle→Preparing→Prepared→Running→Stopping→Stopped |
| 停止 | 资源释放，无崩溃 |

---

## 5. Milestone 3 验证：Linux C++ 音视频采集推流

> **目标**：同时采集音视频，推送到 SRS，验证音视频 PTS 连续性。

### 5.1 推流（视频+音频）

```bash
./build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/test \
  --video-source "testsrc=size=1280x720:rate=30" \
  --enable-audio \
  --audio-source "sine=frequency=440:sample_rate=48000" \
  --log-level info
```

### 5.2 验证

```bash
ffprobe -v quiet -show_streams rtmp://127.0.0.1:1935/live/test | \
  grep -E "codec_name|width|height|sample_rate|channels"
# 预期:
#   codec_name=aac  sample_rate=48000  channels=2
#   codec_name=h264 width=1280  height=720
```

### 5.3 M3 验收

| 检查项 | 通过标准 |
|---|---|
| 编译 | 零警告 |
| 音视频编码 | H.264 + AAC 双轨 |
| ffprobe | 双 codec，48kHz stereo |
| 30 秒稳定 | zero drops，队列水位正常 |
| PTS 交织 | 音视频 PTS 单调，mux 正确 |

---

## 6. 真实设备采集

### 6.1 V4L2 摄像头推流

```bash
# 查看可用设备
v4l2-ctl --list-devices
v4l2-ctl --list-formats-ext -d /dev/video0

# 推流（MJPG 1280x720@30fps）
./build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/camera \
  --video-backend v4l2 \
  --video-source /dev/video0 \
  --video-width 1280 --video-height 720 --video-fps 30 \
  --log-level info
```

### 6.2 ALSA 麦克风推流

```bash
# 查看可用设备
arecord -l

# 推流（ALSA hw:0,0 + lavfi 视频）
./build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/mic \
  --video-source "testsrc=size=1280x720:rate=30" \
  --enable-audio \
  --audio-backend alsa \
  --audio-source hw:0,0 \
  --log-level info
```

### 6.3 完整真实设备推流（摄像头 + 麦克风）

```bash
./build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/full \
  --video-backend v4l2 \
  --video-source /dev/video0 \
  --video-width 1280 --video-height 720 --video-fps 30 \
  --enable-audio \
  --audio-backend alsa \
  --audio-source hw:0,0 \
  --log-level info
```

### 6.4 CLI 参数参考

| 参数 | 说明 | 默认值 |
|---|---|---|
| `--video-backend` | 视频采集后端: `lavfi`(默认), `v4l2`, `file` | lavfi |
| `--audio-backend` | 音频采集后端: `lavfi`(默认), `alsa`, `file` | lavfi |
| `--video-source` | 视频源: lavfi filter/设备路径/文件 | (必填) |
| `--audio-source` | 音频源: lavfi filter/设备名/文件 | sine=... |
| `--enable-audio` | 启用音频采集 | false |

---

## 7. 拉流验证

推流启动后，可以用以下任一方式验证流是否正常。

### 7.1 实时观看（需图形界面）

```bash
# 直接播放 RTMP 流
ffplay rtmp://127.0.0.1:1935/live/test
# 按键: q=退出  f=全屏  p=暂停  s=逐帧
```

### 7.2 检查流参数（无图形界面也可用）

```bash
ffprobe -v quiet -show_streams rtmp://127.0.0.1:1935/live/test | \
  grep -E "codec_name|width|height|sample_rate|channels|r_frame_rate"

# 预期输出:
#   codec_name=aac       (音频轨)
#   sample_rate=48000
#   channels=2
#   codec_name=h264      (视频轨)
#   width=1280
#   height=720
#   r_frame_rate=30/1
```

### 7.3 拉流保存为文件

```bash
# 录制 10 秒到本地文件
timeout 10 ffmpeg -v quiet -i rtmp://127.0.0.1:1935/live/test \
  -t 10 -c copy /tmp/capture.flv

# 然后可以用任意播放器打开，或 scp 到其他机器
ffplay /tmp/capture.flv

# 检查文件完整性
ffprobe -v error -show_entries format=duration,size /tmp/capture.flv
```

### 7.4 不依赖硬件的快速自检

```bash
# 终端 1: 启动推流（lavfi 彩条 + 正弦波，不需要摄像头/麦克风）
./build/app/streambridge_publisher \
  --rtmp-url rtmp://127.0.0.1:1935/live/selftest \
  --video-source "testsrc=size=1280x720:rate=30" \
  --enable-audio \
  --audio-source "sine=frequency=440:sample_rate=48000"

# 终端 2: 验证
ffprobe -v quiet -show_streams rtmp://127.0.0.1:1935/live/selftest | \
  grep -E "codec_name|width|height"
```

### 7.5 拉流端指标采集

```bash
# 使用 ffmpeg 拉流并统计帧/包数
timeout 30 ffmpeg -v info -i rtmp://127.0.0.1:1935/live/test \
  -t 25 -f null - 2>&1 | tail -5
# 输出包含: frame=... fps=... size=... 等信息
```

---

## 8. 故障排查

### 代理不可用

```bash
ss -tlnp | grep 7897
# 无输出则 Clash 未启动，运行: clash-verge
```

### SRS 端口占用

```bash
ss -tlnp | grep 1935
pkill -f "objs/srs"
```

### ffmpeg 命令找不到 libx264

```bash
# 确保用的是静态 FFmpeg，不是系统的 /usr/local/bin/ffmpeg
which ffmpeg
~/workspace/tools/ffmpeg-7.0.2-amd64-static/ffmpeg -encoders 2>/dev/null | grep libx264
```

### 摄像头无法打开

```bash
# 检查设备是否存在
ls -la /dev/video*
v4l2-ctl --list-devices

# 检查权限（可能需要加入 video 组）
groups | grep video || sudo usermod -aG video $USER
```

### ALSA 设备不可用

```bash
# 列出采集设备
arecord -l

# 测试录制
arecord -d 3 -f cd -t wav /tmp/test.wav

# 如果没有 hw:0,0，尝试 default 或 plughw:0,0
```

---

## 8. Android 构建与运行

Android 端禁止 Kotlin，framework 层使用 Java，媒体主链路优先 C++/JNI。当前 Android 工程已完成最小 Java + JNI + C++ native 集成，并使用 NDK 自带 `ndk-build` 构建 native 层。

### 8.1 当前 Android 工程

```text
settings.gradle
build.gradle
android/app/build.gradle
android/app/src/main/AndroidManifest.xml
android/app/src/main/java/com/streambridge/android/MainActivity.java
android/app/src/main/java/com/streambridge/android/NativeBridge.java
android/app/src/main/java/com/streambridge/android/PlaybackEvents.java
android/app/src/main/java/com/streambridge/android/SystemMediaPlayerBackend.java
android/app/src/main/res/values/styles.xml
android/native/Android.mk
android/native/Application.mk
android/native/CMakeLists.txt
android/native/jni/streambridge_jni.cpp
android/native/playback/native_playback_session.h
android/native/playback/native_playback_session.cpp
scripts/android-publish-sample.ps1
```

### 8.2 Windows 构建命令

已验证环境：

- Android Studio JBR：`D:\soft\AS\jbr`
- Android SDK：`D:\soft\AS_sdk`
- Android NDK：`28.2.13676358`
- Android Gradle Plugin：`9.3.0`
- Gradle Wrapper：`9.5.0`
- compileSdk / targetSdk：`37`
- ABI：`arm64-v8a`

```powershell
cmd /c "set JAVA_HOME=D:\soft\AS\jbr&& set ANDROID_HOME=D:\soft\AS_sdk&& set ANDROID_SDK_ROOT=D:\soft\AS_sdk&& set GRADLE_USER_HOME=C:\Users\10177\.gradle&& set PATH=D:\soft\AS\jbr\bin;D:\soft\AS_sdk\platform-tools;%PATH%&& gradlew.bat :android:app:assembleDebug --offline --no-daemon"
```

也可以使用仓库脚本执行 Android 端单边自检：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\android-verify.ps1
```

预期结果：

```text
BUILD SUCCESSFUL
android/app/build/outputs/apk/debug/app-debug.apk
compileDebugKotlin NO-SOURCE
```

### 8.3 真机安装与启动

```powershell
adb -s 84d32674 install -r android\app\build\outputs\apk\debug\app-debug.apk
adb -s 84d32674 shell am start -n com.streambridge.android/.MainActivity
```

如果安装失败：

```text
INSTALL_FAILED_USER_RESTRICTED: Install canceled by user
```

需要在手机开发者选项或系统安全设置中允许 USB 安装、允许未知来源安装，或允许当前调试来源安装应用。当前 APK 已构建成功，真机运行验证只被设备侧安装权限阻塞。

### 8.4 Android 单边验证

Android 播放端不需要等待 Linux 推流端完成，可用本地媒体文件通过 FFmpeg 推流到 SRS，模拟 Linux 端：

```powershell
.\scripts\android-publish-sample.ps1 -InputPath D:\media\sample.mp4 -RtmpUrl rtmp://127.0.0.1/live/stream
```

Android 模拟器访问宿主机 SRS 时，App 内 URL 通常使用：

```text
rtmp://10.0.2.2/live/stream
```

真机访问同一局域网机器时，使用宿主机 LAN IP：

```text
rtmp://<host-lan-ip>/live/stream
```

当前 App 提供两个验证入口：

- `Native Test`：生成公共 `VideoFrame`，通过 C++ `NativeVideoRenderer` 渲染到 `ANativeWindow`，验证 Java/JNI/native/common frame/Surface 链路。
- `Start`：使用 Java `MediaPlayer` 播放输入 URL，验证 UI、Surface 和系统播放后端。

注意：`MediaPlayer` 对 RTMP 的支持取决于设备系统能力。如果设备系统不支持 RTMP，可先用 HTTP MP4/HLS URL 验证 Android 播放 UI、Surface 和音频输出；稳定 RTMP 播放仍需要后续接入 Android FFmpeg ABI。
