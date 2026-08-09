package com.streambridge.android;

import android.view.Surface;

final class NativeBridge {
    static {
        // 按依赖顺序加载：avutil 无依赖，swresample/swscale 依赖 avutil，
        // avcodec 依赖 avutil+swresample，avformat 依赖 avcodec+avutil
        System.loadLibrary("avutil");
        System.loadLibrary("swresample");
        System.loadLibrary("swscale");
        System.loadLibrary("avcodec");
        System.loadLibrary("avformat");
        System.loadLibrary("streambridge_android");
    }

    private long nativeHandle;

    NativeBridge() {
        nativeHandle = nativeCreate();
    }

    int start(String url, Surface surface) {
        if (nativeHandle == 0) {
            return -1;
        }
        return nativeStart(nativeHandle, url, surface);
    }

    void stop() {
        if (nativeHandle != 0) {
            nativeStop(nativeHandle);
        }
    }

    void onSurfaceChanged(Surface surface) {
        if (nativeHandle != 0) {
            nativeSurfaceChanged(nativeHandle, surface);
        }
    }

    void onSurfaceDestroyed() {
        if (nativeHandle != 0) {
            nativeSurfaceDestroyed(nativeHandle);
        }
    }

    String status() {
        if (nativeHandle == 0) {
            return "Released";
        }
        return nativeStatus(nativeHandle);
    }

    void release() {
        if (nativeHandle != 0) {
            nativeRelease(nativeHandle);
            nativeHandle = 0;
        }
    }

    private static native long nativeCreate();

    private static native int nativeStart(long handle, String url, Surface surface);

    private static native void nativeStop(long handle);

    private static native void nativeSurfaceChanged(long handle, Surface surface);

    private static native void nativeSurfaceDestroyed(long handle);

    private static native String nativeStatus(long handle);

    private static native void nativeRelease(long handle);
}
