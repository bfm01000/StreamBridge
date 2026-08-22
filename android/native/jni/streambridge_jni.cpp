#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdint>

#include <memory>
#include <string>
#include <vector>

#include "native_playback_session.h"
#include "native_rtmp_publish_session.h"
#include "video_path_config.h"

namespace {

using streambridge::android::NativePlaybackSession;
using streambridge::android::NativeRtmpPublishSession;

NativePlaybackSession* from_handle(jlong handle) {
    return reinterpret_cast<NativePlaybackSession*>(handle);
}

NativeRtmpPublishSession* publisher_from_handle(jlong handle) {
    return reinterpret_cast<NativeRtmpPublishSession*>(handle);
}

std::string to_string(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

std::vector<uint8_t> to_vector(JNIEnv* env, jbyteArray value) {
    std::vector<uint8_t> data;
    if (value == nullptr) {
        return data;
    }
    const jsize len = env->GetArrayLength(value);
    if (len <= 0) {
        return data;
    }
    data.resize(static_cast<size_t>(len));
    env->GetByteArrayRegion(value, 0, len, reinterpret_cast<jbyte*>(data.data()));
    return data;
}

void append_csd(std::vector<uint8_t>& out, const std::vector<uint8_t>& csd) {
    if (csd.empty()) {
        return;
    }
    out.insert(out.end(), csd.begin(), csd.end());
}

jbyteArray to_jbyte_array(JNIEnv* env, const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(data.size()));
    if (result == nullptr) {
        return nullptr;
    }
    env->SetByteArrayRegion(result,
                            0,
                            static_cast<jsize>(data.size()),
                            reinterpret_cast<const jbyte*>(data.data()));
    return result;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeCreate(JNIEnv*, jclass) {
    auto session = std::make_unique<NativePlaybackSession>();
    return reinterpret_cast<jlong>(session.release());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeStart(
        JNIEnv* env,
        jclass,
        jlong handle,
        jstring url,
        jobject surface,
        jint decode_path) {
    NativePlaybackSession* session = from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    int result = session->start(
        to_string(env, url),
        window,
        streambridge::android::video_decode_path_from_int(decode_path));
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
    return result;
}


extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeStartRtpVideo(
        JNIEnv* env,
        jclass,
        jlong handle,
        jint local_port,
        jobject surface,
        jint width,
        jint height,
        jdouble frame_rate,
        jint decode_path) {
    NativePlaybackSession* session = from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    int result = session->start_rtp_video(
        static_cast<uint16_t>(local_port),
        window,
        width,
        height,
        frame_rate,
        streambridge::android::video_decode_path_from_int(decode_path));
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeStop(JNIEnv*, jclass, jlong handle) {
    NativePlaybackSession* session = from_handle(handle);
    if (session != nullptr) {
        session->stop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeSurfaceChanged(
        JNIEnv* env,
        jclass,
        jlong handle,
        jobject surface) {
    NativePlaybackSession* session = from_handle(handle);
    if (session == nullptr) {
        return;
    }
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    session->set_surface(window);
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeSurfaceDestroyed(JNIEnv*, jclass, jlong handle) {
    NativePlaybackSession* session = from_handle(handle);
    if (session != nullptr) {
        session->clear_surface();
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeStatus(JNIEnv* env, jclass, jlong handle) {
    NativePlaybackSession* session = from_handle(handle);
    if (session == nullptr) {
        return env->NewStringUTF("Released");
    }
    return env->NewStringUTF(session->status_text().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_core_NativeBridge_nativeRelease(JNIEnv*, jclass, jlong handle) {
    delete from_handle(handle);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherCreate(JNIEnv*, jclass) {
    auto session = std::make_unique<NativeRtmpPublishSession>();
    return reinterpret_cast<jlong>(session.release());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherStartVideoOnly(
        JNIEnv* env,
        jclass,
        jlong handle,
        jstring url,
        jint width,
        jint height,
        jint frame_rate,
        jint bitrate_bps,
        jbyteArray csd0,
        jbyteArray csd1) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    std::vector<uint8_t> codec_config;
    append_csd(codec_config, to_vector(env, csd0));
    append_csd(codec_config, to_vector(env, csd1));
    return session->start_video_only(
        to_string(env, url),
        width,
        height,
        frame_rate,
        bitrate_bps,
        codec_config);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherStartRtpVideoOnly(
        JNIEnv* env,
        jclass,
        jlong handle,
        jstring remote_host,
        jint remote_port,
        jint local_port,
        jint width,
        jint height,
        jint frame_rate,
        jint bitrate_bps,
        jbyteArray csd0,
        jbyteArray csd1) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    std::vector<uint8_t> codec_config;
    append_csd(codec_config, to_vector(env, csd0));
    append_csd(codec_config, to_vector(env, csd1));
    return session->start_rtp_video_only(
        to_string(env, remote_host),
        remote_port,
        local_port,
        width,
        height,
        frame_rate,
        bitrate_bps,
        codec_config);
}
extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherStartAv(
        JNIEnv* env,
        jclass,
        jlong handle,
        jstring url,
        jint width,
        jint height,
        jint frame_rate,
        jint video_bitrate_bps,
        jbyteArray video_csd0,
        jbyteArray video_csd1,
        jint sample_rate,
        jint channels,
        jint audio_bitrate_bps,
        jbyteArray audio_csd0) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    std::vector<uint8_t> video_codec_config;
    append_csd(video_codec_config, to_vector(env, video_csd0));
    append_csd(video_codec_config, to_vector(env, video_csd1));
    std::vector<uint8_t> audio_codec_config = to_vector(env, audio_csd0);
    return session->start_av(
        to_string(env, url),
        width,
        height,
        frame_rate,
        video_bitrate_bps,
        video_codec_config,
        sample_rate,
        channels,
        audio_bitrate_bps,
        audio_codec_config);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherStartAudioCapture(
        JNIEnv*,
        jclass,
        jlong handle) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    return session->start_audio_capture();
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherAudioCodecConfig(
        JNIEnv* env,
        jclass,
        jlong handle) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return nullptr;
    }
    return to_jbyte_array(env, session->audio_codec_config());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherWriteVideoPacket(
        JNIEnv* env,
        jclass,
        jlong handle,
        jbyteArray data,
        jlong pts_us,
        jlong dts_us,
        jlong duration_us,
        jboolean key_frame) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    std::vector<uint8_t> packet = to_vector(env, data);
    return session->write_video_packet(
        packet.data(),
        packet.size(),
        pts_us,
        dts_us,
        duration_us,
        key_frame == JNI_TRUE);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherWriteAudioPacket(
        JNIEnv* env,
        jclass,
        jlong handle,
        jbyteArray data,
        jlong pts_us,
        jlong duration_us) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    std::vector<uint8_t> packet = to_vector(env, data);
    return session->write_audio_packet(
        packet.data(),
        packet.size(),
        pts_us,
        duration_us);
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherStop(JNIEnv*, jclass, jlong handle) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session != nullptr) {
        session->stop();
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherStatus(
        JNIEnv* env,
        jclass,
        jlong handle) {
    NativeRtmpPublishSession* session = publisher_from_handle(handle);
    if (session == nullptr) {
        return env->NewStringUTF("PublishReleased");
    }
    return env->NewStringUTF(session->status_text().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_core_NativeBridge_nativePublisherRelease(JNIEnv*, jclass, jlong handle) {
    delete publisher_from_handle(handle);
}
