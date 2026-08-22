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

/**
 * Camera -> OpenGL -> Preview/Encoder 的帧路由器。
 *
 * <p>这个类存在的核心原因是：Camera2 只能直接输出到一个或多个 Surface，但如果直接把相机
 * Surface 分别交给预览和编码器，不同 Surface 的缩放、旋转、裁剪策略可能不一致。这里用
 * SurfaceTexture 接住相机帧，再由 OpenGL 统一绘制到两个目标：</p>
 *
 * <pre>
 * Camera2
 *   -> SurfaceTexture(OES texture)
 *   -> OpenGL draw #1 -> Preview Surface
 *   -> OpenGL draw #2 -> MediaCodec input Surface
 * </pre>
 *
 * <p>这样预览端和推流端看到的是同一张相机纹理、同一套纹理矩阵、同一套缩放逻辑，便于保证
 * “本机预览”和“Linux/ffplay 播放端”画面方向与比例一致。</p>
 */
final class CameraGlFrameRouter {
    private static final String TAG = "StreamBridgeCameraGL";

    // Android 私有 EGL 属性。创建给 MediaCodec input Surface 使用的 EGLSurface 时必须带上
    // recordable 标记，否则部分设备上 eglCreateWindowSurface 可以成功，但编码器拿不到正确帧。
    private static final int EGL_RECORDABLE_ANDROID = 0x3142;

    // 一个覆盖整个 NDC 屏幕的 triangle strip。
    // 每个顶点 4 个 float：x, y, u, v。
    // OpenGL 顶点坐标范围是 [-1, 1]；纹理坐标范围是 [0, 1]。
    // 这里不在顶点里做旋转/裁剪，而是依赖 SurfaceTexture 提供的 textureMatrix 修正相机纹理。
    private static final float[] VERTICES = {
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 1.0f
    };

    // 推流输出分辨率。encoder Surface 的实际尺寸由 MediaCodec 配置决定，这里主要用于日志和诊断。
    private final int streamWidth;
    private final int streamHeight;

    // Camera2 输出给 SurfaceTexture 的 buffer 尺寸。这个尺寸应当和上层选择的相机采集尺寸一致，
    // 否则会出现预览看起来正常但编码端比例异常，或者编码端被系统隐式缩放的情况。
    private final int cameraBufferWidth;
    private final int cameraBufferHeight;

    // 所有 EGL/OpenGL/SurfaceTexture 操作都固定在这一个线程上执行。
    // EGLContext 是线程绑定的；跨线程调用 GL API 很容易导致 EGL_BAD_ACCESS 或状态错乱。
    private HandlerThread glThread;
    private Handler glHandler;

    // EGLDisplay 表示当前进程和系统 EGL 实现之间的连接；EGLContext 保存 GL 状态；
    // EGLSurface 是具体绘制目标，下面分别对应预览 Surface 和编码器 input Surface。
    private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
    private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
    private EGLConfig eglConfig;
    private EGLSurface previewEglSurface = EGL14.EGL_NO_SURFACE;
    private EGLSurface encoderEglSurface = EGL14.EGL_NO_SURFACE;

    // 外部传入的两个输出目标：
    // previewSurface 来自 UI SurfaceView，用于本机预览；
    // encoderSurface 来自 MediaCodec.createInputSurface()，用于送入硬件 H.264 编码器。
    private Surface previewSurface;
    private Surface encoderSurface;

    // 暴露给 Camera2 的输入 Surface。Camera2 实际写入的是 cameraTexture 关联的 OES 纹理。
    private Surface cameraSurface;
    private SurfaceTexture cameraTexture;

    // GL 资源：OES 纹理只能被 samplerExternalOES 采样，不能当普通 2D texture 使用。
    private int oesTextureId;
    private int program;
    private int positionLoc;
    private int texCoordLoc;
    private int texMatrixLoc;
    private FloatBuffer vertexBuffer;

    // SurfaceTexture 每帧都会给出一个 transform matrix，用来处理相机 HAL/BufferQueue 的坐标系差异。
    // 如果忽略这个矩阵，常见现象就是上下颠倒、左右翻转、旋转 90 度或画面被错误裁剪。
    private final float[] textureMatrix = new float[16];
    private final float[] identityMatrix = new float[16];

    private boolean running;

    // 简单的 GL 路由性能统计。这里统计的是 swapBuffers 耗时和帧数，用来判断卡顿发生在
    // OpenGL 绘制/BufferQueue 交换阶段，还是后面的 MediaCodec/RTMP 推流阶段。
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
        // 预览和编码两个 Surface 都必须先准备好。这里不持有 SurfaceView 本身，只持有 Surface，
        // 所以上层需要在 Surface 生命周期变化时 stop 并重新创建路由器。
        if (preview == null || !preview.isValid()) {
            throw new IllegalArgumentException("Preview surface is not ready");
        }
        if (encoder == null || !encoder.isValid()) {
            throw new IllegalArgumentException("Encoder surface is not ready");
        }
        previewSurface = preview;
        encoderSurface = encoder;

        // 单独开 GL 线程，避免在 UI 线程里做 updateTexImage、draw、swapBuffers。
        // 一旦 EGLContext 在这个线程 makeCurrent，后续所有 GL 操作都应该回到这个 Handler 执行。
        glThread = new HandlerThread("StreamBridgeCameraGL");
        glThread.start();
        glHandler = new Handler(glThread.getLooper());

        // initGl() 必须在 GL 线程完成，但 start() 需要同步返回 cameraSurface 给上层 Camera2。
        // CountDownLatch 用来把异步初始化结果同步传回调用者。
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
        // running 先置 false，阻止后续 onFrameAvailable 投递进来的帧继续绘制。
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
            // quitSafely 会处理完队列中已经到达的消息再退出；releaseGl() 上面已经同步执行过，
            // 因此这里主要是关闭 Looper，避免线程泄漏。
            glThread.quitSafely();
            glThread = null;
            glHandler = null;
        }
    }

    private void initGl() {
        // 1. 初始化 EGLDisplay。EGL_DEFAULT_DISPLAY 通常对应系统默认显示/图形栈。
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        checkEgl(eglDisplay != EGL14.EGL_NO_DISPLAY, "eglGetDisplay");
        int[] version = new int[2];
        checkEgl(EGL14.eglInitialize(eglDisplay, version, 0, version, 1),
                "eglInitialize");

        // 2. 选择 EGLConfig。
        // RGB/A 各 8 bit 是常见窗口 Surface 配置；OPENGL_ES2_BIT 表示后面创建 GLES 2.0 context；
        // EGL_RECORDABLE_ANDROID 对编码器 Surface 很关键，表示这个 Surface 可被视频编码器消费。
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

        // 3. 创建 GLES 2.0 context。本类只做 OES 纹理采样和全屏绘制，GLES 2.0 足够。
        int[] contextAttribs = {
                EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL14.EGL_NONE
        };
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig,
                EGL14.EGL_NO_CONTEXT, contextAttribs, 0);
        checkEgl(eglContext != EGL14.EGL_NO_CONTEXT, "eglCreateContext");

        // 4. 把 UI Surface 和 MediaCodec input Surface 都包装成 EGLSurface。
        // 后面每帧会 makeCurrent(previewEglSurface) 画一次，再 makeCurrent(encoderEglSurface) 画一次。
        previewEglSurface = createWindowSurface(previewSurface);
        encoderEglSurface = createWindowSurface(encoderSurface);

        // 先绑定 encoder surface，使后续创建 GL 资源时 context 已经处于 current 状态。
        // GL 资源属于 context，不属于某个特定 EGLSurface，因此之后可以画到两个 Surface。
        makeCurrent(encoderEglSurface);

        // 顶点数据使用 direct buffer，避免 JNI/GL 驱动读取 Java heap 时产生额外复制。
        vertexBuffer = ByteBuffer.allocateDirect(VERTICES.length * 4)
                .order(ByteOrder.nativeOrder())
                .asFloatBuffer();
        vertexBuffer.put(VERTICES).position(0);

        // 编译并查询 shader 变量位置。shader 很简单：顶点 shader 负责套 textureMatrix，
        // fragment shader 从 samplerExternalOES 读取相机纹理。
        program = createProgram();
        positionLoc = GLES20.glGetAttribLocation(program, "aPosition");
        texCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord");
        texMatrixLoc = GLES20.glGetUniformLocation(program, "uTexMatrix");

        // 创建 OES 纹理并交给 SurfaceTexture。Camera2 写入 SurfaceTexture 后，
        // GL 线程通过 updateTexImage() 把最新 BufferQueue 图像绑定到这个纹理 ID。
        oesTextureId = createOesTexture();
        cameraTexture = new SurfaceTexture(oesTextureId);
        cameraTexture.setDefaultBufferSize(cameraBufferWidth, cameraBufferHeight);

        // SurfaceTexture 的帧回调可能来自 BufferQueue 内部线程。这里明确把 renderFrame 投递回 glHandler，
        // 保证 updateTexImage 和所有 GL 绘制都在同一个 EGLContext 线程里执行。
        cameraTexture.setOnFrameAvailableListener(
                texture -> glHandler.post(this::renderFrame), glHandler);

        // 返回给上层 Camera2 使用的 Surface。相机只知道往这个 Surface 写帧，
        // 不需要知道后面还有预览和编码两个输出目标。
        cameraSurface = new Surface(cameraTexture);
        statsStartNs = System.nanoTime();
        android.util.Log.i(TAG, "started stream=" + streamWidth + "x" + streamHeight
                + " cameraBuffer=" + cameraBufferWidth + "x" + cameraBufferHeight);
    }

    private EGLSurface createWindowSurface(Surface surface) {
        // Surface -> EGLSurface 是 Android OpenGL 绘制到 Java/Native Surface 的标准桥接方式。
        // 对 encoderSurface 来说，eglSwapBuffers() 后帧会进入 MediaCodec input queue。
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
            // updateTexImage 会从 SurfaceTexture/BufferQueue 中取出最新相机 buffer，
            // 并把它绑定到 oesTextureId。必须在持有 EGLContext 的 GL 线程调用。
            cameraTexture.updateTexImage();

            // 这是相机帧的生产时间戳，单位 ns。后面写 encoder Surface 时通过
            // eglPresentationTimeANDROID 传给 MediaCodec，最终会影响 BufferInfo.presentationTimeUs。
            long timestampNs = cameraTexture.getTimestamp();

            // 获取当前帧的纹理坐标修正矩阵。它不是普通的模型矩阵，而是 SurfaceTexture 为
            // BufferQueue 坐标系、图像翻转和裁剪区域提供的采样矩阵。
            cameraTexture.getTransformMatrix(textureMatrix);
            inputFrames++;

            // 同一帧画两次：先画到本机预览，再画到编码器输入。
            // 两个输出使用同一个 OES texture 和同一个 textureMatrix，因此画面内容应保持一致。
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

        // 把目标 Surface 绑定为当前线程的 GL draw/read surface。
        // 这一步决定后续 glDrawArrays 的输出去预览窗口，还是去编码器 input Surface。
        makeCurrent(surface);

        // 查询 EGLSurface 当前真实尺寸，而不是假设它等于 streamWidth/streamHeight。
        // 预览 Surface 的尺寸通常来自 UI 布局；编码器 Surface 的尺寸通常来自 MediaCodec 配置。
        int[] width = new int[1];
        int[] height = new int[1];
        EGL14.eglQuerySurface(eglDisplay, surface, EGL14.EGL_WIDTH, width, 0);
        EGL14.eglQuerySurface(eglDisplay, surface, EGL14.EGL_HEIGHT, height, 0);

        // 当前实现是铺满目标 Surface。是否 letterbox/pillarbox 由上层 Surface 尺寸和相机 buffer
        // 选择共同决定；如果后续要严格“不裁剪等比例缩放”，应在这里根据输入/输出宽高比调整 viewport。
        GLES20.glViewport(0, 0, width[0], height[0]);
        GLES20.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);

        // 绑定 shader 和 OES 相机纹理，设置顶点坐标、纹理坐标，然后绘制一个全屏矩形。
        GLES20.glUseProgram(program);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);

        // position 属性读取每个顶点的前两个 float：x/y。
        vertexBuffer.position(0);
        GLES20.glVertexAttribPointer(positionLoc, 2, GLES20.GL_FLOAT,
                false, 16, vertexBuffer);
        GLES20.glEnableVertexAttribArray(positionLoc);

        // texCoord 属性读取每个顶点后两个 float：u/v。
        vertexBuffer.position(2);
        GLES20.glVertexAttribPointer(texCoordLoc, 2, GLES20.GL_FLOAT,
                false, 16, vertexBuffer);
        GLES20.glEnableVertexAttribArray(texCoordLoc);

        // 把 SurfaceTexture 的矩阵传给 shader。这个矩阵非常关键，不能随意替换成 identity，
        // 否则很多设备上相机画面方向会不正确。
        GLES20.glUniformMatrix4fv(texMatrixLoc, 1, false, textureMatrix, 0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        GLES20.glDisableVertexAttribArray(positionLoc);
        GLES20.glDisableVertexAttribArray(texCoordLoc);

        if (encoder) {
            // 只有编码器 Surface 需要 presentation time。MediaCodec 会把这个时间带到输出
            // BufferInfo.presentationTimeUs，后续再映射到 native/common 推流时钟域。
            EGLExt.eglPresentationTimeANDROID(eglDisplay, surface, timestampNs);
        }

        // swapBuffers 对预览 Surface 表示提交到屏幕合成；对编码器 Surface 表示提交给 MediaCodec。
        // 如果这里耗时突然变高，说明可能卡在图形队列、编码器输入队列或系统合成侧。
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
        // 每秒输出一次路由层统计，用来辅助定位“预览流畅但播放端卡顿”这类问题。
        // 如果 preview/encoder 帧数接近 inputFrames，说明 GL 路由基本没有丢帧；
        // 如果 encoderSwapAvgUs/MaxUs 很高，瓶颈可能在编码器输入或 BufferQueue。
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
        // EGLContext 是线程局部状态。调用成功后，本线程后续 GL 命令都会使用 eglContext，
        // 并把绘制结果输出到传入的 EGLSurface。这里 draw surface 和 read surface 使用同一个目标。
        checkEgl(EGL14.eglMakeCurrent(eglDisplay, surface, surface,
                eglContext), "eglMakeCurrent");
    }

    private int createOesTexture() {
        // SurfaceTexture 只能绑定到 GL_TEXTURE_EXTERNAL_OES 纹理。
        // 它的采样器类型是 samplerExternalOES，fragment shader 也必须声明对应 extension。
        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);

        // 相机纹理通常不是普通 mipmap 纹理，使用 LINEAR 过滤可以在缩放时更平滑。
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);

        // OES 外部纹理不能像普通 2D texture 那样自由 repeat，边缘固定 clamp。
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        checkGl("createOesTexture");
        return textures[0];
    }

    private int createProgram() {
        // 顶点 shader：
        // - aPosition 直接作为 gl_Position，画满整个目标 Surface；
        // - aTexCoord 先乘 SurfaceTexture 提供的 uTexMatrix，再传给 fragment shader。
        String vertexShader =
                "attribute vec4 aPosition;\n"
                        + "attribute vec4 aTexCoord;\n"
                        + "uniform mat4 uTexMatrix;\n"
                        + "varying vec2 vTexCoord;\n"
                        + "void main() {\n"
                        + "  gl_Position = aPosition;\n"
                        + "  vTexCoord = (uTexMatrix * aTexCoord).xy;\n"
                        + "}\n";

        // 片元 shader：
        // - GL_OES_EGL_image_external 允许 shader 直接采样 Android 外部图像；
        // - sTexture 对应上面绑定的 camera OES texture。
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
        // shader 编译错误通常来自设备 GLES 版本、extension 不支持或 shader 文本拼写问题。
        // 这里保留完整 driver log，方便从 logcat 直接定位。
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
        // 资源释放也回到 GL 线程执行，保证释放纹理、program、EGLSurface 时 context 状态一致。
        // 释放顺序大致按“上层包装 -> GL 资源 -> EGL 绑定/Surface/Context/Display”逆序清理。
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
            // 先解绑当前线程上的 context/surface，再销毁 EGLSurface 和 EGLContext。
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
        // EGL 错误码只有在失败后立即读取才可靠，所以这里统一封装检查。
        if (!ok) {
            throw new IllegalStateException(label + " failed eglError=0x"
                    + Integer.toHexString(EGL14.eglGetError()));
        }
    }

    private void checkGl(String label) {
        // GL 错误是 sticky 的；如果前面某个 GL 调用失败，这里能尽早暴露出来。
        int error = GLES20.glGetError();
        if (error != GLES20.GL_NO_ERROR) {
            throw new IllegalStateException(label + " failed glError=0x"
                    + Integer.toHexString(error));
        }
    }

    private static final class GlStartupResult {
        // 用于把 GL 线程里的同步初始化结果传回 start() 调用线程。
        Surface cameraSurface;
        Exception error;
    }
}
