#include <android/native_window_jni.h>
#include <jni.h>

#include <memory>
#include <string>

#include "native_playback_session.h"

namespace {

using streambridge::android::NativePlaybackSession;

NativePlaybackSession* from_handle(jlong handle) {
    return reinterpret_cast<NativePlaybackSession*>(handle);
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

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_streambridge_android_NativeBridge_nativeCreate(JNIEnv*, jclass) {
    auto session = std::make_unique<NativePlaybackSession>();
    return reinterpret_cast<jlong>(session.release());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_streambridge_android_NativeBridge_nativeStart(
        JNIEnv* env,
        jclass,
        jlong handle,
        jstring url,
        jobject surface) {
    NativePlaybackSession* session = from_handle(handle);
    if (session == nullptr) {
        return -1;
    }
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    int result = session->start(to_string(env, url), window);
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_NativeBridge_nativeStop(JNIEnv*, jclass, jlong handle) {
    NativePlaybackSession* session = from_handle(handle);
    if (session != nullptr) {
        session->stop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_NativeBridge_nativeSurfaceChanged(
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
Java_com_streambridge_android_NativeBridge_nativeSurfaceDestroyed(JNIEnv*, jclass, jlong handle) {
    NativePlaybackSession* session = from_handle(handle);
    if (session != nullptr) {
        session->clear_surface();
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_streambridge_android_NativeBridge_nativeStatus(JNIEnv* env, jclass, jlong handle) {
    NativePlaybackSession* session = from_handle(handle);
    if (session == nullptr) {
        return env->NewStringUTF("Released");
    }
    return env->NewStringUTF(session->status_text().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_streambridge_android_NativeBridge_nativeRelease(JNIEnv*, jclass, jlong handle) {
    delete from_handle(handle);
}
