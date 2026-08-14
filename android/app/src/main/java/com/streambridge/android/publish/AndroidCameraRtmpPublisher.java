package com.streambridge.android.publish;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.graphics.Rect;
import android.view.Surface;
import android.view.WindowManager;

import java.nio.ByteBuffer;
import java.util.Arrays;

import com.streambridge.android.core.BuildInfo;
import com.streambridge.android.core.NativeBridge;

public final class AndroidCameraRtmpPublisher {
    public interface Events {
        void onPublishStatus(String message);
        void onPublishError(String message);
    }

    private static final String MIME_AVC = "video/avc";
    private static final String TAG = "StreamBridgePublisher";
    private static final int WIDTH = 1280;
    private static final int HEIGHT = 720;
    private static final int TARGET_LONG_SIDE = 960;
    private static final int TARGET_SHORT_SIDE = 720;
    private static final int FRAME_RATE = 30;
    private static final int BITRATE_BPS = 2_000_000;
    private static final int I_FRAME_INTERVAL_SEC = 2;

    private final Context context;
    private final NativeBridge nativeBridge;
    private final Events events;

    private HandlerThread cameraThread;
    private Handler cameraHandler;
    private MediaCodec encoder;
    private Surface encoderInputSurface;
    private Surface cameraInputSurface;
    private Surface previewSurface;
    private CameraGlFrameRouter glFrameRouter;
    private CameraDevice cameraDevice;
    private CameraCharacteristics cameraCharacteristics;
    private String selectedCameraId;
    private CameraCaptureSession captureSession;
    private Thread encoderThread;
    private volatile boolean running;
    private volatile boolean publisherStarted;
    private String publishUrl;
    private long encoderStatsStartMs;
    private long encoderOutputFrames;
    private long encoderOutputBytes;
    private long encoderKeyFrames;
    private long encoderDequeueTimeouts;
    private long encoderLastPtsUs = Long.MIN_VALUE;
    private long encoderPtsGapMaxUs;
    private int activeWidth = WIDTH;
    private int activeHeight = HEIGHT;
    private int activeBitrateBps = BITRATE_BPS;
    private int sensorOrientationDegrees;
    private int displayRotationDegrees;
    private int streamRotationDegrees;

    public AndroidCameraRtmpPublisher(Context context, NativeBridge nativeBridge, Events events) {
        this.context = context;
        this.nativeBridge = nativeBridge;
        this.events = events;
    }

    public boolean isRunning() {
        return running;
    }

    public void start(String url, Surface preview) {
        if (running) {
            events.onPublishError("Publisher already running");
            return;
        }
        if (preview == null || !preview.isValid()) {
            events.onPublishError("Preview surface is not ready");
            return;
        }
        if (context.checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            events.onPublishError("Camera permission missing");
            return;
        }
        publishUrl = url;
        previewSurface = preview;
        resetFlowStats();
        publisherStarted = false;
        running = true;
        android.util.Log.i(TAG, "start VERSION=" + BuildInfo.VERSION);
        try {
            prepareCameraGeometry();
            startEncoder();
            startGlRouter();
            startCamera();
            events.onPublishStatus("Camera publish starting");
        } catch (Exception e) {
            events.onPublishError("Camera publish start failed: " + e.getMessage());
            stop();
        }
    }

    public void stop() {
        running = false;
        closeCamera();
        stopGlRouter();
        stopEncoder();
        nativeBridge.stopPublish();
        publisherStarted = false;
        previewSurface = null;
        events.onPublishStatus("Camera publish stopped");
    }

    public String status() {
        return nativeBridge.publishStatus();
    }

    public int[] previewAspectForCurrentCamera() {
        try {
            prepareCameraGeometry();
            return new int[] { activeWidth, activeHeight };
        } catch (Exception e) {
            android.util.Log.w(TAG, "preview aspect fallback: " + e.getMessage());
            return new int[] { WIDTH, HEIGHT };
        }
    }

    private void startEncoder() throws Exception {
        boolean portrait = activeHeight > activeWidth;
        int primaryWidth = activeWidth;
        int primaryHeight = activeHeight;
        int fallbackWidth = portrait ? 480 : 640;
        int fallbackHeight = portrait ? 640 : 480;
        int fullFrameFallbackWidth = portrait ? 720 : 960;
        int fullFrameFallbackHeight = portrait ? 960 : 720;
        int lastResortWidth = portrait ? HEIGHT : WIDTH;
        int lastResortHeight = portrait ? WIDTH : HEIGHT;
        EncoderConfig[] configs = new EncoderConfig[] {
                new EncoderConfig(primaryWidth, primaryHeight, BITRATE_BPS, true, true),
                new EncoderConfig(primaryWidth, primaryHeight, BITRATE_BPS, true, false),
                new EncoderConfig(primaryWidth, primaryHeight, BITRATE_BPS, false, false),
                new EncoderConfig(fullFrameFallbackWidth, fullFrameFallbackHeight,
                        1_500_000, false, false),
                new EncoderConfig(fallbackWidth, fallbackHeight, 1_000_000, false, false),
                new EncoderConfig(lastResortWidth, lastResortHeight, 1_500_000, false, false)
        };

        Exception lastError = null;
        for (EncoderConfig config : configs) {
            try {
                android.util.Log.i(TAG, "try encoder config " + config);
                configureEncoder(config);
                activeWidth = config.width;
                activeHeight = config.height;
                activeBitrateBps = config.bitrateBps;
                android.util.Log.i(TAG, "active encoder config width=" + activeWidth
                        + " height=" + activeHeight
                        + " fps=" + FRAME_RATE
                        + " bitrate=" + activeBitrateBps
                        + " inputSurfaceValid=" + (encoderInputSurface != null
                        && encoderInputSurface.isValid()));
                events.onPublishStatus("Encoder configured "
                        + activeWidth + "x" + activeHeight);
                break;
            } catch (Exception e) {
                lastError = e;
                android.util.Log.w(TAG, "encoder config failed " + config + ": "
                        + e.getClass().getSimpleName() + " " + e.getMessage());
                releaseEncoder();
            }
        }
        if (encoder == null) {
            throw lastError != null ? lastError : new IllegalStateException("No AVC encoder");
        }

        encoderThread = new Thread(this::drainEncoder, "StreamBridgeCameraEncoder");
        encoderThread.start();
    }

    private void configureEncoder(EncoderConfig config) throws Exception {
        MediaFormat format = MediaFormat.createVideoFormat(MIME_AVC, config.width, config.height);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE, config.bitrateBps);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, FRAME_RATE);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL_SEC);
        if (config.useCbr) {
            format.setInteger(MediaFormat.KEY_BITRATE_MODE,
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
        }
        if (config.useBaselineProfile) {
            format.setInteger(MediaFormat.KEY_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
        }

        encoder = MediaCodec.createEncoderByType(MIME_AVC);
        encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        encoderInputSurface = encoder.createInputSurface();
        encoder.start();
    }

    private void startGlRouter() throws Exception {
        glFrameRouter = new CameraGlFrameRouter(
                activeWidth,
                activeHeight,
                activeWidth,
                activeHeight);
        cameraInputSurface = glFrameRouter.start(previewSurface, encoderInputSurface);
        android.util.Log.i(TAG, "OpenGL router connected cameraInputSurfaceValid="
                + (cameraInputSurface != null && cameraInputSurface.isValid())
                + " previewSurfaceValid=" + (previewSurface != null && previewSurface.isValid())
                + " encoderSurfaceValid=" + (encoderInputSurface != null
                && encoderInputSurface.isValid())
                + " stream=" + activeWidth + "x" + activeHeight);
    }

    private void startCamera() throws CameraAccessException {
        cameraThread = new HandlerThread("StreamBridgeCamera");
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());

        CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
        if (selectedCameraId == null || cameraCharacteristics == null) {
            prepareCameraGeometry();
        }
        if (selectedCameraId == null) {
            throw new CameraAccessException(CameraAccessException.CAMERA_ERROR,
                    "No usable camera");
        }
        Integer facing = cameraCharacteristics.get(CameraCharacteristics.LENS_FACING);
        Rect activeArray = cameraCharacteristics.get(
                CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        android.util.Log.i(TAG, "selected cameraId=" + selectedCameraId
                + " facing=" + lensFacingName(facing)
                + " sensorActiveArray=" + rectToString(activeArray)
                + " targetEncoder=" + activeWidth + "x" + activeHeight
                + " sensorOrientation=" + sensorOrientationDegrees
                + " displayRotation=" + displayRotationDegrees
                + " streamRotation=" + streamRotationDegrees
                + " cameraInputSurfaceValid=" + (cameraInputSurface != null
                && cameraInputSurface.isValid()));
        manager.openCamera(selectedCameraId, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice camera) {
                cameraDevice = camera;
                createCaptureSession();
            }

            @Override
            public void onDisconnected(CameraDevice camera) {
                events.onPublishError("Camera disconnected");
                camera.close();
                cameraDevice = null;
                stop();
            }

            @Override
            public void onError(CameraDevice camera, int error) {
                events.onPublishError("Camera error: " + error);
                camera.close();
                cameraDevice = null;
                stop();
            }
        }, cameraHandler);
    }

    private void prepareCameraGeometry() throws CameraAccessException {
        CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
        selectedCameraId = chooseCamera(manager);
        if (selectedCameraId == null) {
            throw new CameraAccessException(CameraAccessException.CAMERA_ERROR,
                    "No usable camera");
        }
        cameraCharacteristics = manager.getCameraCharacteristics(selectedCameraId);
        Integer sensorOrientation =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
        sensorOrientationDegrees = sensorOrientation == null ? 0 : sensorOrientation;
        displayRotationDegrees = currentDisplayRotationDegrees();
        streamRotationDegrees = normalizeDegrees(sensorOrientationDegrees - displayRotationDegrees);
        Rect activeArray =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        boolean portraitStream = streamRotationDegrees == 90 || streamRotationDegrees == 270;
        int sensorWidth = activeArray == null ? WIDTH : activeArray.width();
        int sensorHeight = activeArray == null ? HEIGHT : activeArray.height();
        int orientedWidth = portraitStream ? sensorHeight : sensorWidth;
        int orientedHeight = portraitStream ? sensorWidth : sensorHeight;
        int[] streamSize = fullFrameStreamSize(orientedWidth, orientedHeight);
        activeWidth = streamSize[0];
        activeHeight = streamSize[1];
        activeBitrateBps = BITRATE_BPS;
        android.util.Log.i(TAG, "camera geometry cameraId=" + selectedCameraId
                + " sensorOrientation=" + sensorOrientationDegrees
                + " displayRotation=" + displayRotationDegrees
                + " streamRotation=" + streamRotationDegrees
                + " sensorActiveArray=" + rectToString(activeArray)
                + " orientedSensor=" + orientedWidth + "x" + orientedHeight
                + " streamSize=" + activeWidth + "x" + activeHeight);
    }

    private int[] fullFrameStreamSize(int orientedWidth, int orientedHeight) {
        if (orientedWidth <= 0 || orientedHeight <= 0) {
            return new int[] { WIDTH, HEIGHT };
        }
        boolean portrait = orientedHeight >= orientedWidth;
        int longSide = TARGET_LONG_SIDE;
        int shortSide = (int) Math.round(
                longSide * (double) Math.min(orientedWidth, orientedHeight)
                        / (double) Math.max(orientedWidth, orientedHeight));
        longSide = alignDown(longSide, 16);
        shortSide = alignDown(clamp(shortSide, 480, TARGET_SHORT_SIDE), 16);
        return portrait
                ? new int[] { shortSide, longSide }
                : new int[] { longSide, shortSide };
    }

    private int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private int alignDown(int value, int alignment) {
        return Math.max(alignment, value - (value % alignment));
    }

    private String chooseCamera(CameraManager manager) throws CameraAccessException {
        String fallback = null;
        for (String id : manager.getCameraIdList()) {
            CameraCharacteristics c = manager.getCameraCharacteristics(id);
            Integer facing = c.get(CameraCharacteristics.LENS_FACING);
            if (fallback == null) {
                fallback = id;
            }
            if (facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT) {
                return id;
            }
        }
        return fallback;
    }

    private int currentDisplayRotationDegrees() {
        WindowManager windowManager =
                (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        int rotation = windowManager == null ? Surface.ROTATION_0
                : windowManager.getDefaultDisplay().getRotation();
        switch (rotation) {
            case Surface.ROTATION_90:
                return 90;
            case Surface.ROTATION_180:
                return 180;
            case Surface.ROTATION_270:
                return 270;
            case Surface.ROTATION_0:
            default:
                return 0;
        }
    }

    private int normalizeDegrees(int degrees) {
        int normalized = degrees % 360;
        return normalized < 0 ? normalized + 360 : normalized;
    }

    private void createCaptureSession() {
        try {
            if (cameraDevice == null || cameraInputSurface == null) {
                return;
            }
            cameraDevice.createCaptureSession(Arrays.asList(cameraInputSurface),
                    new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession session) {
                            captureSession = session;
                            try {
                                CaptureRequest.Builder request =
                                        cameraDevice.createCaptureRequest(
                                                CameraDevice.TEMPLATE_RECORD);
                                request.addTarget(cameraInputSurface);
                                android.util.Log.i(TAG, "capture session configured "
                                        + "cameraInputSurfaceValid=" + cameraInputSurface.isValid()
                                        + " scalerCrop=disabled"
                                        + " route=Camera->OpenGL->Preview+Encoder"
                                        + " encoder=" + activeWidth + "x" + activeHeight);
                                request.set(CaptureRequest.CONTROL_MODE,
                                        CaptureRequest.CONTROL_MODE_AUTO);
                                captureSession.setRepeatingRequest(
                                        request.build(), null, cameraHandler);
                                events.onPublishStatus("Camera publish running");
                            } catch (CameraAccessException e) {
                                events.onPublishError("Camera request failed: " + e.getMessage());
                                stop();
                            }
                        }

                        @Override
                        public void onConfigureFailed(CameraCaptureSession session) {
                            events.onPublishError("Camera session configure failed");
                            stop();
                        }
                    }, cameraHandler);
        } catch (CameraAccessException e) {
            events.onPublishError("Create camera session failed: " + e.getMessage());
            stop();
        }
    }

    private void drainEncoder() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long frameDurationUs = 1_000_000L / FRAME_RATE;
        while (running) {
            int index;
            try {
                index = encoder.dequeueOutputBuffer(info, 50_000);
            } catch (Exception e) {
                if (running) {
                    events.onPublishError("Encoder dequeue failed: " + e.getMessage());
                }
                return;
            }

            if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
                encoderDequeueTimeouts++;
                maybeLogEncoderStats();
                continue;
            }
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat output = encoder.getOutputFormat();
                int outputWidth = output.containsKey(MediaFormat.KEY_WIDTH)
                        ? output.getInteger(MediaFormat.KEY_WIDTH) : -1;
                int outputHeight = output.containsKey(MediaFormat.KEY_HEIGHT)
                        ? output.getInteger(MediaFormat.KEY_HEIGHT) : -1;
                int outputStride = output.containsKey("stride")
                        ? output.getInteger("stride") : -1;
                int outputSliceHeight = output.containsKey("slice-height")
                        ? output.getInteger("slice-height") : -1;
                android.util.Log.i(TAG, "encoder output format width=" + outputWidth
                        + " height=" + outputHeight
                        + " stride=" + outputStride
                        + " sliceHeight=" + outputSliceHeight
                        + " publishHeader=" + activeWidth + "x" + activeHeight);
                byte[] csd0 = byteBufferToArray(output.getByteBuffer("csd-0"));
                byte[] csd1 = byteBufferToArray(output.getByteBuffer("csd-1"));
                int result = nativeBridge.startVideoPublish(
                        publishUrl,
                        activeWidth,
                        activeHeight,
                        FRAME_RATE,
                        activeBitrateBps,
                        csd0,
                        csd1);
                if (result != 0) {
                    events.onPublishError("Native publisher start failed: "
                            + result + " " + nativeBridge.publishStatus());
                    running = false;
                    return;
                }
                publisherStarted = true;
                events.onPublishStatus("RTMP publisher started "
                        + activeWidth + "x" + activeHeight);
                continue;
            }
            if (index < 0) {
                continue;
            }

            try {
                ByteBuffer outputBuffer = encoder.getOutputBuffer(index);
                if (outputBuffer != null
                        && info.size > 0
                        && publisherStarted
                        && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                    outputBuffer.position(info.offset);
                    outputBuffer.limit(info.offset + info.size);
                    byte[] packet = new byte[info.size];
                    outputBuffer.get(packet);
                    boolean keyFrame = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                    recordEncoderOutput(info.size, info.presentationTimeUs, keyFrame);
                    int writeResult = nativeBridge.writeVideoPacket(
                            packet,
                            info.presentationTimeUs,
                            info.presentationTimeUs,
                            frameDurationUs,
                            keyFrame);
                    if (writeResult != 0) {
                        events.onPublishError("Native publisher enqueue failed: "
                                + writeResult + " " + nativeBridge.publishStatus());
                        running = false;
                    }
                    maybeLogEncoderStats();
                }
            } finally {
                encoder.releaseOutputBuffer(index, false);
            }

            if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                running = false;
            }
        }
    }

    private void resetFlowStats() {
        long nowMs = System.currentTimeMillis();
        encoderStatsStartMs = nowMs;
        encoderOutputFrames = 0;
        encoderOutputBytes = 0;
        encoderKeyFrames = 0;
        encoderDequeueTimeouts = 0;
        encoderLastPtsUs = Long.MIN_VALUE;
        encoderPtsGapMaxUs = 0;
    }

    private void recordEncoderOutput(int size, long ptsUs, boolean keyFrame) {
        encoderOutputFrames++;
        encoderOutputBytes += size;
        if (keyFrame) {
            encoderKeyFrames++;
        }
        if (encoderLastPtsUs != Long.MIN_VALUE) {
            long gapUs = Math.abs(ptsUs - encoderLastPtsUs);
            encoderPtsGapMaxUs = Math.max(encoderPtsGapMaxUs, gapUs);
        }
        encoderLastPtsUs = ptsUs;
    }

    private void maybeLogEncoderStats() {
        long nowMs = System.currentTimeMillis();
        long elapsedMs = nowMs - encoderStatsStartMs;
        if (elapsedMs < 1_000L) {
            return;
        }
        long bitrateKbps = elapsedMs <= 0 ? 0 : encoderOutputBytes * 8L / elapsedMs;
        android.util.Log.i(TAG, "flow encoder frames=" + encoderOutputFrames
                + " key=" + encoderKeyFrames
                + " bytes=" + encoderOutputBytes
                + " bitrateKbps=" + bitrateKbps
                + " dequeueTimeouts=" + encoderDequeueTimeouts
                + " ptsGapMaxUs=" + encoderPtsGapMaxUs
                + " native=" + nativeBridge.publishStatus()
                + " elapsedMs=" + elapsedMs);
        encoderStatsStartMs = nowMs;
        encoderOutputFrames = 0;
        encoderOutputBytes = 0;
        encoderKeyFrames = 0;
        encoderDequeueTimeouts = 0;
        encoderPtsGapMaxUs = 0;
    }

    private byte[] byteBufferToArray(ByteBuffer buffer) {
        if (buffer == null) {
            return new byte[0];
        }
        ByteBuffer dup = buffer.duplicate();
        dup.position(0);
        byte[] data = new byte[dup.remaining()];
        dup.get(data);
        return data;
    }

    private String lensFacingName(Integer facing) {
        if (facing == null) {
            return "null";
        }
        if (facing == CameraCharacteristics.LENS_FACING_FRONT) {
            return "FRONT";
        }
        if (facing == CameraCharacteristics.LENS_FACING_BACK) {
            return "BACK";
        }
        if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) {
            return "EXTERNAL";
        }
        return String.valueOf(facing);
    }

    private String rectToString(Rect rect) {
        if (rect == null) {
            return "null";
        }
        return rect.left + "," + rect.top + "-" + rect.right + "," + rect.bottom
                + " (" + rect.width() + "x" + rect.height() + ")";
    }

    private void closeCamera() {
        try {
            if (captureSession != null) {
                captureSession.stopRepeating();
                captureSession.close();
            }
        } catch (Exception ignored) {
        }
        captureSession = null;
        if (cameraDevice != null) {
            cameraDevice.close();
            cameraDevice = null;
        }
        if (cameraThread != null) {
            cameraThread.quitSafely();
            cameraThread = null;
            cameraHandler = null;
        }
    }

    private void stopEncoder() {
        if (encoder != null) {
            try {
                encoder.signalEndOfInputStream();
            } catch (Exception ignored) {
            }
        }
        if (encoderThread != null) {
            try {
                encoderThread.join(1_000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            encoderThread = null;
        }
        releaseEncoder();
    }

    private void stopGlRouter() {
        if (glFrameRouter != null) {
            glFrameRouter.stop();
            glFrameRouter = null;
        }
        cameraInputSurface = null;
    }

    private void releaseEncoder() {
        if (encoder != null) {
            try {
                encoder.stop();
            } catch (Exception ignored) {
            }
            try {
                encoder.release();
            } catch (Exception ignored) {
            }
            encoder = null;
        }
        if (encoderInputSurface != null) {
            encoderInputSurface.release();
            encoderInputSurface = null;
        }
    }

    private static final class EncoderConfig {
        final int width;
        final int height;
        final int bitrateBps;
        final boolean useCbr;
        final boolean useBaselineProfile;

        EncoderConfig(int width,
                      int height,
                      int bitrateBps,
                      boolean useCbr,
                      boolean useBaselineProfile) {
            this.width = width;
            this.height = height;
            this.bitrateBps = bitrateBps;
            this.useCbr = useCbr;
            this.useBaselineProfile = useBaselineProfile;
        }

        @Override
        public String toString() {
            return width + "x" + height
                    + " bitrate=" + bitrateBps
                    + " cbr=" + useCbr
                    + " baseline=" + useBaselineProfile;
        }
    }

}
