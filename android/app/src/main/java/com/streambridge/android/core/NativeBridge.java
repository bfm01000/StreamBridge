package com.streambridge.android.core;

import android.view.Surface;

public final class NativeBridge {
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
    private long publisherHandle;

    public NativeBridge() {
        nativeHandle = nativeCreate();
        publisherHandle = nativePublisherCreate();
    }

    public static final int PATH_AUTO = 0;
    public static final int PATH_MEDIACODEC_AHB_GPU = 1;
    public static final int PATH_MEDIACODEC_SURFACE = 2;
    public static final int PATH_FFMPEG_SOFTWARE = 3;

    public int start(String url, Surface surface, int decodePath) {
        if (nativeHandle == 0) {
            return -1;
        }
        return nativeStart(nativeHandle, url, surface, decodePath);
    }

    public void stop() {
        if (nativeHandle != 0) {
            nativeStop(nativeHandle);
        }
    }

    public void onSurfaceChanged(Surface surface) {
        if (nativeHandle != 0) {
            nativeSurfaceChanged(nativeHandle, surface);
        }
    }

    public void onSurfaceDestroyed() {
        if (nativeHandle != 0) {
            nativeSurfaceDestroyed(nativeHandle);
        }
    }

    public String status() {
        if (nativeHandle == 0) {
            return "Released";
        }
        return nativeStatus(nativeHandle);
    }

    public int startVideoPublish(String url, int width, int height, int frameRate, int bitrateBps,
                                 byte[] csd0, byte[] csd1) {
        if (publisherHandle == 0) {
            return -1;
        }
        return nativePublisherStartVideoOnly(
                publisherHandle, url, width, height, frameRate, bitrateBps, csd0, csd1);
    }

    public int startAvPublish(String url,
                              int width,
                              int height,
                              int frameRate,
                              int videoBitrateBps,
                              byte[] videoCsd0,
                              byte[] videoCsd1,
                              int sampleRate,
                              int channels,
                              int audioBitrateBps,
                              byte[] audioCsd0) {
        if (publisherHandle == 0) {
            return -1;
        }
        return nativePublisherStartAv(
                publisherHandle, url, width, height, frameRate, videoBitrateBps,
                videoCsd0, videoCsd1, sampleRate, channels, audioBitrateBps, audioCsd0);
    }

    public int startPublishAudioCapture() {
        if (publisherHandle == 0) {
            return -1;
        }
        return nativePublisherStartAudioCapture(publisherHandle);
    }

    public byte[] publishAudioCodecConfig() {
        if (publisherHandle == 0) {
            return null;
        }
        return nativePublisherAudioCodecConfig(publisherHandle);
    }

    public int writeVideoPacket(byte[] data, long ptsUs, long dtsUs,
                                long durationUs, boolean keyFrame) {
        if (publisherHandle == 0) {
            return -1;
        }
        return nativePublisherWriteVideoPacket(
                publisherHandle, data, ptsUs, dtsUs, durationUs, keyFrame);
    }

    public int writeAudioPacket(byte[] data, long ptsUs, long durationUs) {
        if (publisherHandle == 0) {
            return -1;
        }
        return nativePublisherWriteAudioPacket(publisherHandle, data, ptsUs, durationUs);
    }

    public void stopPublish() {
        if (publisherHandle != 0) {
            nativePublisherStop(publisherHandle);
        }
    }

    public String publishStatus() {
        if (publisherHandle == 0) {
            return "PublishReleased";
        }
        return nativePublisherStatus(publisherHandle);
    }

    public void release() {
        if (publisherHandle != 0) {
            nativePublisherRelease(publisherHandle);
            publisherHandle = 0;
        }
        if (nativeHandle != 0) {
            nativeRelease(nativeHandle);
            nativeHandle = 0;
        }
    }

    private static native long nativeCreate();

    private static native int nativeStart(long handle, String url, Surface surface, int decodePath);

    private static native void nativeStop(long handle);

    private static native void nativeSurfaceChanged(long handle, Surface surface);

    private static native void nativeSurfaceDestroyed(long handle);

    private static native String nativeStatus(long handle);

    private static native void nativeRelease(long handle);

    private static native long nativePublisherCreate();

    private static native int nativePublisherStartVideoOnly(long handle, String url,
            int width, int height, int frameRate, int bitrateBps, byte[] csd0, byte[] csd1);

    private static native int nativePublisherStartAv(long handle, String url,
            int width, int height, int frameRate, int videoBitrateBps,
            byte[] videoCsd0, byte[] videoCsd1,
            int sampleRate, int channels, int audioBitrateBps, byte[] audioCsd0);

    private static native int nativePublisherStartAudioCapture(long handle);

    private static native byte[] nativePublisherAudioCodecConfig(long handle);

    private static native int nativePublisherWriteVideoPacket(long handle, byte[] data,
            long ptsUs, long dtsUs, long durationUs, boolean keyFrame);

    private static native int nativePublisherWriteAudioPacket(long handle, byte[] data,
            long ptsUs, long durationUs);

    private static native void nativePublisherStop(long handle);

    private static native String nativePublisherStatus(long handle);

    private static native void nativePublisherRelease(long handle);
}
