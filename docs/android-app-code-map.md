# Android App 代码导览

本文用于快速理清 Android 端 App 的主要模块、职责、调用链路和数据流转。当前 Android 端同时支持：

1. RTMP 拉流播放：Linux/SRS 推流，Android 拉流、解封装、解码、同步、渲染和音频播放。
2. Android 采集推流：Android 摄像头和麦克风采集，编码后通过 RTMP 推给 SRS/Linux。

Android 端禁止 Kotlin。当前 Java 只负责 UI、权限、生命周期和少量 Android Framework 控制；媒体链路尽量进入 native/common。

## 1. 顶层目录

```text
android/
├── app/src/main/java/com/streambridge/android/
│   ├── ui/          # Activity、SurfaceView、页面与按钮逻辑
│   ├── core/        # NativeBridge、BuildInfo
│   ├── playback/    # Java fallback 播放封装
│   └── publish/     # Android 相机推流 Java 控制层、OpenGL 路由
└── native/
    ├── jni/         # Java <-> C++ JNI 边界
    ├── playback/    # native 拉流播放链路
    ├── publish/     # native 推流链路、native 音频采集编码
    ├── platform/    # Android 日志适配
    └── libs/        # Android arm64 FFmpeg 动态库
```

公共逻辑来自：

```text
common/include/streambridge/
common/src/
```

Android native 通过 `android/native/Android.mk` 编译，当前实际构建系统是 `ndk-build`。`android/native/CMakeLists.txt` 存在，但 Gradle 任务当前走 `Android.mk`。

## 2. Java 层模块

### `MainActivity`

文件：[MainActivity.java](../android/app/src/main/java/com/streambridge/android/ui/MainActivity.java)

职责：

- 创建主页、拉流页、推流页。
- 处理按钮、URL 输入、Spinner、状态和指标显示。
- 管理 `SurfaceHolder.Callback`。
- 申请 Camera 和 Record Audio 权限。
- 调用 `NativeBridge` 启停 native 拉流。
- 调用 `AndroidCameraRtmpPublisher` 启停 Android 采集推流。
- 提供 TCP 测试，确认手机能访问 SRS 的 `1935` 端口。

主要入口：

```text
showHomePage()
showPlaybackPage()
showPublishPage()
startPlayback()
stopPlayback()
toggleCameraPublish()
pollNativeStatus()
```

### `AspectRatioSurfaceView`

文件：[AspectRatioSurfaceView.java](../android/app/src/main/java/com/streambridge/android/ui/AspectRatioSurfaceView.java)

职责：

- 让播放画面和推流预览按指定宽高比显示。
- 推流预览页通过它避免相机画面被拉伸。

### `NativeBridge`

文件：[NativeBridge.java](../android/app/src/main/java/com/streambridge/android/core/NativeBridge.java)

职责：

- 加载 FFmpeg 和 `streambridge_android` native 动态库。
- 持有两个 native handle：
  - `nativeHandle`：拉流播放 `NativePlaybackSession`。
  - `publisherHandle`：推流 `NativeRtmpPublishSession`。
- 暴露少量稳定 JNI API 给 Java。

拉流相关：

```text
start(url, surface, decodePath)
stop()
onSurfaceChanged(surface)
onSurfaceDestroyed()
status()
```

推流相关：

```text
startPublishAudioCapture()
publishAudioCodecConfig()
startAvPublish(...)
writeVideoPacket(...)
stopPublish()
publishStatus()
```

注意：当前视频编码仍在 Java `MediaCodec`，所以视频 packet 会通过 `writeVideoPacket()` 送入 native。音频采集和 AAC 编码已经迁到 native，不再逐包从 Java 传音频。

### `AndroidCameraRtmpPublisher`

文件：[AndroidCameraRtmpPublisher.java](../android/app/src/main/java/com/streambridge/android/publish/AndroidCameraRtmpPublisher.java)

职责：

- 管理 Android 推流会话。
- 选择前置摄像头。
- 计算相机方向、预览比例和编码尺寸。
- 创建视频 `MediaCodec` H.264 encoder。
- 创建 `CameraGlFrameRouter`，把 Camera 画面同时送到预览 Surface 和 encoder Surface。
- 启动 native 音频采集编码。
- 等待视频 SPS/PPS 和 native AAC config 都准备好后，启动 native AV RTMP publisher。
- 将编码后的视频 packet 写入 native。

核心启动顺序：

```text
start(url, previewSurface)
  -> prepareCameraGeometry()
  -> startEncoder()
  -> startGlRouter()
  -> startNativeAudioCapture()
  -> startCamera()
```

AV publisher 启动条件：

```text
videoCsd0 + videoCsd1 ready
audioCsd0 ready from native
  -> nativeBridge.startAvPublish(...)
  -> publisherStarted = true
```

### `CameraGlFrameRouter`

文件：[CameraGlFrameRouter.java](../android/app/src/main/java/com/streambridge/android/publish/CameraGlFrameRouter.java)

职责：

- 创建 `SurfaceTexture` 给 Camera2 输出。
- 使用 OpenGL ES 把同一帧画到两个 Surface：
  - App 预览 Surface。
  - H.264 encoder input Surface。
- 负责等比例缩放和避免预览/推流画面不一致。
- 通过 `EGLExt.eglPresentationTimeANDROID()` 给 encoder Surface 设置相机帧时间。

数据流：

```text
Camera2
  -> SurfaceTexture / OES texture
  -> OpenGL draw
     -> preview Surface
     -> encoder input Surface
```

### `SystemMediaPlayerBackend`

文件：[SystemMediaPlayerBackend.java](../android/app/src/main/java/com/streambridge/android/playback/SystemMediaPlayerBackend.java)

职责：

- Java `MediaPlayer` fallback。
- 当前 RTMP 主路径不依赖它；RTMP 使用 native FFmpeg 播放链路。

## 3. JNI 边界

文件：[streambridge_jni.cpp](../android/native/jni/streambridge_jni.cpp)

JNI 只做薄封装：

- Java 字符串、byte array 和 native 类型转换。
- 创建/释放 native session。
- 把 Surface 转成 `ANativeWindow`。
- 调用 C++ session 方法。

拉流 handle：

```text
nativeCreate()
  -> new NativePlaybackSession

nativeStart(handle, url, surface, decodePath)
  -> NativePlaybackSession::start(...)

nativeStop(handle)
  -> NativePlaybackSession::stop()
```

推流 handle：

```text
nativePublisherCreate()
  -> new NativeRtmpPublishSession

nativePublisherStartAudioCapture()
  -> NativeRtmpPublishSession::start_audio_capture()

nativePublisherAudioCodecConfig()
  -> NativeRtmpPublishSession::audio_codec_config()

nativePublisherStartAv(...)
  -> NativeRtmpPublishSession::start_av(...)

nativePublisherWriteVideoPacket(...)
  -> NativeRtmpPublishSession::write_video_packet(...)
```

## 4. Android 拉流播放链路

### 主要 native 模块

```text
NativePlaybackSession
DemuxWorker
VideoDecodeWorker
AudioDecodeWorker
NativeVideoRenderer
NativeAudioOutput
PlaybackMetrics
PlaybackReconnectController
MediaCodecVideoDecoder
FFmpegSubscriber / FFmpeg decoders in common
```

关键文件：

- [native_playback_session.cpp](../android/native/playback/native_playback_session.cpp)
- [demux_worker.cpp](../android/native/playback/demux_worker.cpp)
- [video_decode_worker.cpp](../android/native/playback/video_decode_worker.cpp)
- [audio_decode_worker.cpp](../android/native/playback/audio_decode_worker.cpp)
- [native_video_renderer.cpp](../android/native/playback/native_video_renderer.cpp)
- [native_audio_output.cpp](../android/native/playback/native_audio_output.cpp)
- [mediacodec_video_decoder.cpp](../android/native/playback/mediacodec/mediacodec_video_decoder.cpp)

### 拉流调用链

```text
MainActivity.startPlayback()
  -> NativeBridge.start(url, surface, decodePath)
  -> JNI nativeStart(...)
  -> NativePlaybackSession::start(url, window, path)
  -> start demux_thread
  -> start video_thread
  -> start audio_thread
```

### 拉流数据流

```text
RTMP URL
  -> FFmpegSubscriber
  -> DemuxWorker
  -> video packet queue
  -> VideoDecodeWorker
  -> decoder path:
       AUTO / MEDIACODEC_AHB_GPU / MEDIACODEC_SURFACE / FFMPEG_SOFTWARE
  -> AVSyncController + MediaClock
  -> NativeVideoRenderer / Surface

RTMP URL
  -> FFmpegSubscriber
  -> DemuxWorker
  -> audio packet queue
  -> AudioDecodeWorker
  -> FFmpeg AAC decoder
  -> NativeAudioOutput / AAudio
  -> MediaClock audio master update
```

### 拉流时钟和同步

公共同步逻辑：

- [av_sync.h](../common/include/streambridge/av_sync.h)
- [av_sync.cpp](../common/src/core/av_sync.cpp)

播放端默认音频主时钟：

```text
NativeAudioOutput played frames
  -> MediaClock.update_audio(...)
  -> VideoDecodeWorker compares video pts with MediaClock.now()
  -> Wait / Render / RenderLate / Drop
```

没有音频时，播放端会用视频首帧启动主时钟，避免 video-only 流一直等待。

### 解码路径

UI Spinner 对应：

```text
AUTO
MEDIACODEC_AHB_GPU
MEDIACODEC_SURFACE
FFMPEG_SOFTWARE
```

大致含义：

- `MEDIACODEC_SURFACE`：MediaCodec 直接输出到 Surface。
- `MEDIACODEC_AHB_GPU`：MediaCodec 输出 `AImage/AHardwareBuffer`，再走 GPU/渲染路径。
- `FFMPEG_SOFTWARE`：FFmpeg 软件解码后交给 native renderer。
- `AUTO`：按能力选择。

## 5. Android 采集推流链路

### 主要模块

Java：

```text
MainActivity
AndroidCameraRtmpPublisher
CameraGlFrameRouter
NativeBridge
```

Native：

```text
NativeRtmpPublishSession
NativeAudioAacEncoder
FFmpegRTMPPublisher
PublishTimestampAligner
MediaQueue<MediaPacket>
```

关键文件：

- [AndroidCameraRtmpPublisher.java](../android/app/src/main/java/com/streambridge/android/publish/AndroidCameraRtmpPublisher.java)
- [CameraGlFrameRouter.java](../android/app/src/main/java/com/streambridge/android/publish/CameraGlFrameRouter.java)
- [native_rtmp_publish_session.cpp](../android/native/publish/native_rtmp_publish_session.cpp)
- [native_audio_aac_encoder.cpp](../android/native/publish/native_audio_aac_encoder.cpp)
- [ffmpeg_rtmp_publisher.cpp](../common/src/ffmpeg/ffmpeg_rtmp_publisher.cpp)
- [publish_timestamp_aligner.h](../common/include/streambridge/publish_timestamp_aligner.h)

### 推流启动调用链

```text
MainActivity.toggleCameraPublish()
  -> check CAMERA + RECORD_AUDIO permission
  -> bindActiveNetworkForRtmp()
  -> AndroidCameraRtmpPublisher.start(url, previewSurface)
     -> prepareCameraGeometry()
     -> startEncoder()
        -> Java MediaCodec H.264 encoder
     -> startGlRouter()
        -> CameraGlFrameRouter creates Camera input Surface
     -> startNativeAudioCapture()
        -> NativeBridge.startPublishAudioCapture()
        -> JNI nativePublisherStartAudioCapture()
        -> NativeRtmpPublishSession::start_audio_capture()
        -> NativeAudioAacEncoder::start()
     -> startCamera()
        -> Camera2 repeating request to GL input Surface
```

### 推流视频数据流

```text
Camera2 front camera
  -> CameraGlFrameRouter SurfaceTexture
  -> OpenGL draw
     -> preview Surface
     -> MediaCodec encoder input Surface
  -> Java MediaCodec H.264 output
  -> AndroidCameraRtmpPublisher.drainEncoder()
  -> NativeBridge.writeVideoPacket(...)
  -> JNI nativePublisherWriteVideoPacket(...)
  -> NativeRtmpPublishSession::write_video_packet(...)
  -> PublishTimestampAligner
  -> MediaQueue<MediaPacket>
  -> writer_loop()
  -> FFmpegRTMPPublisher.write_packet()
  -> FLV video tag
  -> RTMP
```

视频格式：

```text
Codec: H.264
Frame rate: 30 fps
Bitrate: default 2 Mbps, with fallback configs
Input: MediaCodec Surface
```

### 推流音频数据流

```text
AAudio input
  -> NativeAudioAacEncoder
  -> AMediaCodec AAC encoder
  -> AAC csd-0
     -> NativeRtmpPublishSession::audio_codec_config()
     -> Java polls NativeBridge.publishAudioCodecConfig()
     -> startAvPublish(...)
  -> AAC packet
     -> NativeRtmpPublishSession::on_native_audio_packet()
     -> write_audio_packet(...)
     -> PublishTimestampAligner
     -> MediaQueue<MediaPacket>
     -> writer_loop()
     -> FFmpegRTMPPublisher.write_packet()
     -> FLV audio tag
     -> RTMP
```

音频格式：

```text
Capture: AAudio PCM S16
Encode: AMediaCodec AAC-LC
Sample rate: 48000 Hz
Channels: 1
Bitrate: 96000 bps
```

音频 PTS：

```text
AAudioStream_getTimestamp(CLOCK_MONOTONIC)
  -> capture_pts_us
  -> AMediaCodec queue input PTS
  -> AAC output presentationTimeUs
  -> MediaPacket.pts
```

如果 `AAudioStream_getTimestamp()` 暂时不可用，会 fallback 到 common 的 `monotonic_now_us()`。

### AV Header 启动条件

RTMP/FLV header 需要同时知道视频和音频 codec config：

```text
Video:
  MediaCodec INFO_OUTPUT_FORMAT_CHANGED
  -> csd-0 SPS
  -> csd-1 PPS

Audio:
  NativeAudioAacEncoder AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
  -> csd-0 AudioSpecificConfig

Both ready:
  -> NativeBridge.startAvPublish(...)
  -> NativeRtmpPublishSession::start_av(...)
  -> FFmpegRTMPPublisher.write_header(video, audio)
```

这就是为什么 Java 侧会有 `tryStartNativePublisher()`：它不是启动音频编码，而是在等 AV config 齐全后启动 RTMP muxer。

## 6. 推流时间戳对齐

公共类：[publish_timestamp_aligner.h](../common/include/streambridge/publish_timestamp_aligner.h)

职责：

- 音频和视频都用采集时间戳进入 native。
- 等音视频首包都出现。
- 以较晚的首包 PTS 作为 `base_pts`。
- 丢弃早于 `base_pts` 的启动阶段 packet。
- 后续所有 packet 做 `pts -= base_pts`，从 0 开始写入 RTMP。
- 统计 `av_diff_us = latest_video_pts - latest_audio_pts`。

Android 推流端调用位置：

```text
NativeRtmpPublishSession::write_video_packet()
NativeRtmpPublishSession::write_audio_packet()
```

状态里可以看到：

```text
alignDrop=...
avDiffUs=...
```

## 7. RTMP/FLV 写出

公共实现：[ffmpeg_rtmp_publisher.cpp](../common/src/ffmpeg/ffmpeg_rtmp_publisher.cpp)

职责：

- 打开 RTMP URL。
- 写 FLV header。
- 写 H.264 sequence header。
- 写 AAC sequence header。
- 写 video/audio packet。
- 统计 bytes、packets。

Android 推流和 Linux 推流应该复用这部分，不应各写一套 RTMP。

## 8. 线程模型

### 拉流播放

```text
UI thread
  -> start/stop/status/surface callback

demux_thread
  -> RTMP read / demux
  -> packet queues

video_thread
  -> video decode
  -> AV sync decision
  -> render

audio_thread
  -> audio decode
  -> AAudio output
  -> update MediaClock
```

### Android 推流

```text
UI thread
  -> button / permission / status

camera HandlerThread
  -> Camera2 repeating request

GL thread
  -> SurfaceTexture update
  -> draw preview and encoder surfaces

Java encoder thread
  -> drain H.264 MediaCodec output
  -> writeVideoPacket JNI

Native audio thread
  -> AAudio read
  -> AMediaCodec AAC encode
  -> write_audio_packet

Native RTMP writer thread
  -> pop MediaPacket
  -> FFmpegRTMPPublisher.write_packet()
```

## 9. 状态和日志

常用 tag：

```text
StreamBridgeUI
StreamBridgePublisher
NativeRtmpPublishSession
NativeAudioAacEncoder
StreamBridgeMC
video/audio worker logs
```

推流正常时关注：

```text
BuildInfo VERSION=android-2026-08-17-native-audio-publish
native audio capture requested sampleRate=48000 channels=1 bitrate=96000
native audio config ready bytes=...
RTMP AV publisher started ...
Publishing queued=... written=... q=... alignDrop=... avDiffUs=...
```

拉流正常时关注：

```text
Native playback started
demux opened
video decoder opened
audio frames increasing
rendered increasing
av_diff stable
drop not continuously increasing
```

## 10. 常见入口速查

### Android 播放一个 RTMP URL

```text
MainActivity.startPlayback()
  -> NativeBridge.start()
  -> streambridge_jni.cpp nativeStart()
  -> NativePlaybackSession::start()
```

### Android 停止播放

```text
MainActivity.stopPlayback()
  -> NativeBridge.stop()
  -> NativePlaybackSession::stop()
```

### Android 开始推流

```text
MainActivity.toggleCameraPublish()
  -> AndroidCameraRtmpPublisher.start()
  -> Java video encoder + CameraGlFrameRouter
  -> NativeBridge.startPublishAudioCapture()
  -> NativeBridge.startAvPublish()
```

### Android 停止推流

```text
AndroidCameraRtmpPublisher.stop()
  -> closeCamera()
  -> stopGlRouter()
  -> stopEncoder()
  -> NativeBridge.stopPublish()
  -> NativeRtmpPublishSession::stop()
  -> stop native audio + RTMP writer
```

## 11. 当前边界和后续优化方向

当前设计状态：

- 拉流播放主链路基本在 native/common。
- 推流音频已经迁到 native。
- 推流视频编码仍在 Java `MediaCodec`，视频 packet 逐包 JNI 传入 native。
- OpenGL 路由仍在 Java 层，负责预览和 encoder Surface 同源。
- RTMP/FLV 写出复用 common。
- 推流时间戳对齐已经有 common `PublishTimestampAligner`。

后续可以优化：

1. 将视频编码从 Java `MediaCodec` 迁到 native `AMediaCodec`。
2. 将 Camera 输入进一步封装为 native/平台适配层，Java 只保留权限和生命周期。
3. 让 Linux 推流端也接入 `PublishTimestampAligner`，统一推流端 AV 起点对齐。
4. 将推流端指标展示到 UI：`audio_pts`、`video_pts`、`avDiffUs`、`alignDrop`、写包耗时、码率。
5. 更新 [android-camera-publish-validation.md](android-camera-publish-validation.md)，让验证文档和 native 音频推流现状一致。

