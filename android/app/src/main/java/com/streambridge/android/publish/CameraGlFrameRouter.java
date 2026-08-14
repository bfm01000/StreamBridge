package com.streambridge.android.publish;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.EGLExt;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.opengl.Matrix;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class CameraGlFrameRouter {
    private static final String TAG = "StreamBridgeCameraGL";
    private static final int EGL_RECORDABLE_ANDROID = 0x3142;

    private static final float[] VERTICES = {
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 1.0f
    };

    private final int streamWidth;
    private final int streamHeight;
    private final int cameraBufferWidth;
    private final int cameraBufferHeight;

    private HandlerThread glThread;
    private Handler glHandler;
    private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
    private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
    private EGLConfig eglConfig;
    private EGLSurface previewEglSurface = EGL14.EGL_NO_SURFACE;
    private EGLSurface encoderEglSurface = EGL14.EGL_NO_SURFACE;
    private Surface previewSurface;
    private Surface encoderSurface;
    private Surface cameraSurface;
    private SurfaceTexture cameraTexture;
    private int oesTextureId;
    private int program;
    private int positionLoc;
    private int texCoordLoc;
    private int texMatrixLoc;
    private FloatBuffer vertexBuffer;
    private final float[] textureMatrix = new float[16];
    private final float[] identityMatrix = new float[16];
    private boolean running;
    private long statsStartNs;
    private long inputFrames;
    private long previewFrames;
    private long encoderFrames;
    private long previewSwapTotalUs;
    private long encoderSwapTotalUs;
    private long previewSwapMaxUs;
    private long encoderSwapMaxUs;

    CameraGlFrameRouter(int streamWidth,
                        int streamHeight,
                        int cameraBufferWidth,
                        int cameraBufferHeight) {
        this.streamWidth = streamWidth;
        this.streamHeight = streamHeight;
        this.cameraBufferWidth = cameraBufferWidth;
        this.cameraBufferHeight = cameraBufferHeight;
        Matrix.setIdentityM(identityMatrix, 0);
    }

    Surface start(Surface preview, Surface encoder) throws Exception {
        if (preview == null || !preview.isValid()) {
            throw new IllegalArgumentException("Preview surface is not ready");
        }
        if (encoder == null || !encoder.isValid()) {
            throw new IllegalArgumentException("Encoder surface is not ready");
        }
        previewSurface = preview;
        encoderSurface = encoder;
        glThread = new HandlerThread("StreamBridgeCameraGL");
        glThread.start();
        glHandler = new Handler(glThread.getLooper());

        CountDownLatch latch = new CountDownLatch(1);
        GlStartupResult result = new GlStartupResult();
        glHandler.post(() -> {
            try {
                initGl();
                running = true;
                result.cameraSurface = cameraSurface;
            } catch (Exception e) {
                result.error = e;
            } finally {
                latch.countDown();
            }
        });
        if (!latch.await(5, TimeUnit.SECONDS)) {
            throw new IllegalStateException("OpenGL startup timed out");
        }
        if (result.error != null) {
            throw result.error;
        }
        return result.cameraSurface;
    }

    void stop() {
        running = false;
        if (glHandler != null) {
            CountDownLatch latch = new CountDownLatch(1);
            glHandler.post(() -> {
                releaseGl();
                latch.countDown();
            });
            try {
                latch.await(2, TimeUnit.SECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        if (glThread != null) {
            glThread.quitSafely();
            glThread = null;
            glHandler = null;
        }
    }

    private void initGl() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        checkEgl(eglDisplay != EGL14.EGL_NO_DISPLAY, "eglGetDisplay");
        int[] version = new int[2];
        checkEgl(EGL14.eglInitialize(eglDisplay, version, 0, version, 1),
                "eglInitialize");
        int[] configAttribs = {
                EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL_RECORDABLE_ANDROID, 1,
                EGL14.EGL_NONE
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] numConfigs = new int[1];
        checkEgl(EGL14.eglChooseConfig(eglDisplay, configAttribs, 0,
                configs, 0, configs.length, numConfigs, 0), "eglChooseConfig");
        if (numConfigs[0] <= 0) {
            throw new IllegalStateException("No EGL config");
        }
        eglConfig = configs[0];
        int[] contextAttribs = {
                EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL14.EGL_NONE
        };
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig,
                EGL14.EGL_NO_CONTEXT, contextAttribs, 0);
        checkEgl(eglContext != EGL14.EGL_NO_CONTEXT, "eglCreateContext");

        previewEglSurface = createWindowSurface(previewSurface);
        encoderEglSurface = createWindowSurface(encoderSurface);
        makeCurrent(encoderEglSurface);

        vertexBuffer = ByteBuffer.allocateDirect(VERTICES.length * 4)
                .order(ByteOrder.nativeOrder())
                .asFloatBuffer();
        vertexBuffer.put(VERTICES).position(0);
        program = createProgram();
        positionLoc = GLES20.glGetAttribLocation(program, "aPosition");
        texCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord");
        texMatrixLoc = GLES20.glGetUniformLocation(program, "uTexMatrix");
        oesTextureId = createOesTexture();
        cameraTexture = new SurfaceTexture(oesTextureId);
        cameraTexture.setDefaultBufferSize(cameraBufferWidth, cameraBufferHeight);
        cameraTexture.setOnFrameAvailableListener(
                texture -> glHandler.post(this::renderFrame), glHandler);
        cameraSurface = new Surface(cameraTexture);
        statsStartNs = System.nanoTime();
        android.util.Log.i(TAG, "started stream=" + streamWidth + "x" + streamHeight
                + " cameraBuffer=" + cameraBufferWidth + "x" + cameraBufferHeight);
    }

    private EGLSurface createWindowSurface(Surface surface) {
        int[] surfaceAttribs = { EGL14.EGL_NONE };
        EGLSurface eglSurface = EGL14.eglCreateWindowSurface(
                eglDisplay, eglConfig, surface, surfaceAttribs, 0);
        checkEgl(eglSurface != EGL14.EGL_NO_SURFACE, "eglCreateWindowSurface");
        return eglSurface;
    }

    private void renderFrame() {
        if (!running || cameraTexture == null) {
            return;
        }
        try {
            cameraTexture.updateTexImage();
            long timestampNs = cameraTexture.getTimestamp();
            cameraTexture.getTransformMatrix(textureMatrix);
            inputFrames++;
            drawToSurface(previewEglSurface, false, timestampNs);
            drawToSurface(encoderEglSurface, true, timestampNs);
            maybeLogStats();
        } catch (Exception e) {
            android.util.Log.e(TAG, "render failed: " + e.getMessage());
        }
    }

    private void drawToSurface(EGLSurface surface, boolean encoder, long timestampNs) {
        if (surface == EGL14.EGL_NO_SURFACE) {
            return;
        }
        makeCurrent(surface);
        int[] width = new int[1];
        int[] height = new int[1];
        EGL14.eglQuerySurface(eglDisplay, surface, EGL14.EGL_WIDTH, width, 0);
        EGL14.eglQuerySurface(eglDisplay, surface, EGL14.EGL_HEIGHT, height, 0);
        GLES20.glViewport(0, 0, width[0], height[0]);
        GLES20.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glUseProgram(program);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);
        vertexBuffer.position(0);
        GLES20.glVertexAttribPointer(positionLoc, 2, GLES20.GL_FLOAT,
                false, 16, vertexBuffer);
        GLES20.glEnableVertexAttribArray(positionLoc);
        vertexBuffer.position(2);
        GLES20.glVertexAttribPointer(texCoordLoc, 2, GLES20.GL_FLOAT,
                false, 16, vertexBuffer);
        GLES20.glEnableVertexAttribArray(texCoordLoc);
        GLES20.glUniformMatrix4fv(texMatrixLoc, 1, false, textureMatrix, 0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        GLES20.glDisableVertexAttribArray(positionLoc);
        GLES20.glDisableVertexAttribArray(texCoordLoc);
        if (encoder) {
            EGLExt.eglPresentationTimeANDROID(eglDisplay, surface, timestampNs);
        }
        long beforeSwapNs = System.nanoTime();
        EGL14.eglSwapBuffers(eglDisplay, surface);
        long swapUs = (System.nanoTime() - beforeSwapNs) / 1_000L;
        if (encoder) {
            encoderFrames++;
            encoderSwapTotalUs += swapUs;
            encoderSwapMaxUs = Math.max(encoderSwapMaxUs, swapUs);
        } else {
            previewFrames++;
            previewSwapTotalUs += swapUs;
            previewSwapMaxUs = Math.max(previewSwapMaxUs, swapUs);
        }
    }

    private void maybeLogStats() {
        long nowNs = System.nanoTime();
        long elapsedNs = nowNs - statsStartNs;
        if (elapsedNs < 1_000_000_000L) {
            return;
        }
        long elapsedMs = Math.max(1L, elapsedNs / 1_000_000L);
        long previewAvgUs = previewFrames == 0 ? 0 : previewSwapTotalUs / previewFrames;
        long encoderAvgUs = encoderFrames == 0 ? 0 : encoderSwapTotalUs / encoderFrames;
        android.util.Log.i(TAG, "stats in=" + inputFrames
                + " preview=" + previewFrames
                + " encoder=" + encoderFrames
                + " elapsedMs=" + elapsedMs
                + " previewSwapAvgUs=" + previewAvgUs
                + " previewSwapMaxUs=" + previewSwapMaxUs
                + " encoderSwapAvgUs=" + encoderAvgUs
                + " encoderSwapMaxUs=" + encoderSwapMaxUs
                + " stream=" + streamWidth + "x" + streamHeight);
        statsStartNs = nowNs;
        inputFrames = 0;
        previewFrames = 0;
        encoderFrames = 0;
        previewSwapTotalUs = 0;
        encoderSwapTotalUs = 0;
        previewSwapMaxUs = 0;
        encoderSwapMaxUs = 0;
    }

    private void makeCurrent(EGLSurface surface) {
        checkEgl(EGL14.eglMakeCurrent(eglDisplay, surface, surface,
                eglContext), "eglMakeCurrent");
    }

    private int createOesTexture() {
        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        checkGl("createOesTexture");
        return textures[0];
    }

    private int createProgram() {
        String vertexShader =
                "attribute vec4 aPosition;\n"
                        + "attribute vec4 aTexCoord;\n"
                        + "uniform mat4 uTexMatrix;\n"
                        + "varying vec2 vTexCoord;\n"
                        + "void main() {\n"
                        + "  gl_Position = aPosition;\n"
                        + "  vTexCoord = (uTexMatrix * aTexCoord).xy;\n"
                        + "}\n";
        String fragmentShader =
                "#extension GL_OES_EGL_image_external : require\n"
                        + "precision mediump float;\n"
                        + "varying vec2 vTexCoord;\n"
                        + "uniform samplerExternalOES sTexture;\n"
                        + "void main() {\n"
                        + "  gl_FragColor = texture2D(sTexture, vTexCoord);\n"
                        + "}\n";
        int vertex = compileShader(GLES20.GL_VERTEX_SHADER, vertexShader);
        int fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentShader);
        int linkedProgram = GLES20.glCreateProgram();
        GLES20.glAttachShader(linkedProgram, vertex);
        GLES20.glAttachShader(linkedProgram, fragment);
        GLES20.glLinkProgram(linkedProgram);
        int[] linkStatus = new int[1];
        GLES20.glGetProgramiv(linkedProgram, GLES20.GL_LINK_STATUS, linkStatus, 0);
        if (linkStatus[0] == 0) {
            String log = GLES20.glGetProgramInfoLog(linkedProgram);
            GLES20.glDeleteProgram(linkedProgram);
            throw new IllegalStateException("Program link failed: " + log);
        }
        GLES20.glDeleteShader(vertex);
        GLES20.glDeleteShader(fragment);
        return linkedProgram;
    }

    private int compileShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        int[] compiled = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, compiled, 0);
        if (compiled[0] == 0) {
            String log = GLES20.glGetShaderInfoLog(shader);
            GLES20.glDeleteShader(shader);
            throw new IllegalStateException("Shader compile failed: " + log);
        }
        return shader;
    }

    private void releaseGl() {
        if (cameraSurface != null) {
            cameraSurface.release();
            cameraSurface = null;
        }
        if (cameraTexture != null) {
            cameraTexture.release();
            cameraTexture = null;
        }
        if (oesTextureId != 0) {
            GLES20.glDeleteTextures(1, new int[] { oesTextureId }, 0);
            oesTextureId = 0;
        }
        if (program != 0) {
            GLES20.glDeleteProgram(program);
            program = 0;
        }
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE,
                    EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (previewEglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, previewEglSurface);
                previewEglSurface = EGL14.EGL_NO_SURFACE;
            }
            if (encoderEglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, encoderEglSurface);
                encoderEglSurface = EGL14.EGL_NO_SURFACE;
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext);
                eglContext = EGL14.EGL_NO_CONTEXT;
            }
            EGL14.eglReleaseThread();
            EGL14.eglTerminate(eglDisplay);
            eglDisplay = EGL14.EGL_NO_DISPLAY;
        }
        android.util.Log.i(TAG, "stopped");
    }

    private void checkEgl(boolean ok, String label) {
        if (!ok) {
            throw new IllegalStateException(label + " failed eglError=0x"
                    + Integer.toHexString(EGL14.eglGetError()));
        }
    }

    private void checkGl(String label) {
        int error = GLES20.glGetError();
        if (error != GLES20.GL_NO_ERROR) {
            throw new IllegalStateException(label + " failed glError=0x"
                    + Integer.toHexString(error));
        }
    }

    private static final class GlStartupResult {
        Surface cameraSurface;
        Exception error;
    }
}
