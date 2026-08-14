LOCAL_PATH := $(call my-dir)

# ========== FFmpeg prebuilt shared libraries ==========
# 每个 .so 已设置 DT_SONAME（如 libavcodec.so），链接器将使用 SONAME
# 而非 Windows 路径写入 DT_NEEDED。

include $(CLEAR_VARS)
LOCAL_MODULE := avutil
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libavutil.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swresample
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libswresample.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := swscale
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libswscale.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avcodec
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libavcodec.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := avformat
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libavformat.so
include $(PREBUILT_SHARED_LIBRARY)

# ========== StreamBridge native library ==========

include $(CLEAR_VARS)

LOCAL_MODULE := streambridge_android
LOCAL_SRC_FILES := \
    jni/streambridge_jni.cpp \
    platform/android_logging.cpp \
    playback/audio_decode_worker.cpp \
    playback/demux_worker.cpp \
    playback/hardware_buffer_frame.cpp \
    playback/mediacodec/mediacodec_csd.cpp \
    playback/mediacodec/mediacodec_video_decoder.cpp \
    playback/native_audio_output.cpp \
    playback/native_playback_session.cpp \
    playback/native_video_renderer.cpp \
    playback/playback_metrics.cpp \
    playback/playback_queue_config.cpp \
    playback/playback_reconnect_controller.cpp \
    playback/video_path_config.cpp \
    playback/video_decode_worker.cpp \
    ../../common/src/av_sync.cpp \
    ../../common/src/ffmpeg_utils.cpp \
    ../../common/src/ffmpeg/codec_config.cpp \
    ../../common/src/ffmpeg/ffmpeg_video_decoder.cpp \
    ../../common/src/ffmpeg/ffmpeg_audio_decoder.cpp \
    ../../common/src/ffmpeg/ffmpeg_subscriber.cpp \
    ../../common/src/session_utils.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/playback \
    $(LOCAL_PATH)/../../common/include \
    $(LOCAL_PATH)/../../common/src \
    $(LOCAL_PATH)/../../third_party/ffmpeg-android/arm64-v8a/include

LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -Werror -DSTREAMBRIDGE_BUILD_VERSION=\"2026-08-11-57\"
LOCAL_LDLIBS := -laaudio -landroid -lEGL -lGLESv2 -llog -lmediandk -lnativewindow

LOCAL_SHARED_LIBRARIES := avformat avcodec swscale swresample avutil

include $(BUILD_SHARED_LIBRARY)
