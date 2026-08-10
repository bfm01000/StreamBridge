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
    playback/ffmpeg/ffmpeg_subscriber.cpp \
    playback/ffmpeg/ffmpeg_video_decoder.cpp \
    playback/ffmpeg/ffmpeg_audio_decoder.cpp \
    playback/native_audio_output.cpp \
    playback/native_playback_session.cpp \
    playback/native_video_renderer.cpp \
    ../../common/src/av_sync.cpp \
    ../../common/src/session_utils.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/playback \
    $(LOCAL_PATH)/../../common/include \
    $(LOCAL_PATH)/../../third_party/ffmpeg-android/arm64-v8a/include

LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -Werror -DSTREAMBRIDGE_BUILD_VERSION=\"2026-08-10-36\"
LOCAL_LDLIBS := -laaudio -landroid -llog

LOCAL_SHARED_LIBRARIES := avformat avcodec swscale swresample avutil

include $(BUILD_SHARED_LIBRARY)
