package com.streambridge.android.ui;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Bundle;
import android.graphics.Rect;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import com.streambridge.android.core.BuildInfo;
import com.streambridge.android.core.NativeBridge;
import com.streambridge.android.playback.PlaybackEvents;
import com.streambridge.android.playback.SystemMediaPlayerBackend;
import com.streambridge.android.publish.AndroidCameraRtmpPublisher;

public final class MainActivity extends Activity implements SurfaceHolder.Callback,
        PlaybackEvents, AndroidCameraRtmpPublisher.Events {
    private static final String DEFAULT_PLAY_URL = "rtmp://192.168.31.57:1935/live/test";
    private static final String DEFAULT_PUBLISH_URL =
            "rtmp://192.168.31.57:1935/live/android_camera";
    private static final String DEFAULT_RTP_REMOTE_HOST = "192.168.31.57";
    private static final int DEFAULT_RTP_REMOTE_PORT = 5004;
    private static final int DEFAULT_RTP_LOCAL_PORT = 0;
    private static final int DEFAULT_RTP_PLAY_WIDTH = 1280;
    private static final int DEFAULT_RTP_PLAY_HEIGHT = 720;
    private static final double DEFAULT_RTP_PLAY_FRAME_RATE = 30.0;
    private static final int REQUEST_PUBLISH_PERMISSIONS = 2001;

    private static final String[] STREAM_SOURCE_LABELS = {
            "Custom URL",
            "Camera /live/camera",
            "File /live/file",
            "Selftest /live/selftest",
            "Test /live/test",
            "Full AV /live/full"
    };
    private static final String[] STREAM_SOURCE_PATHS = {
            null,
            "/live/camera",
            "/live/file",
            "/live/selftest",
            "/live/test",
            "/live/full"
    };

    private NativeBridge nativeBridge;
    private AndroidCameraRtmpPublisher cameraPublisher;
    private SystemMediaPlayerBackend mediaPlayerBackend;

    private AspectRatioSurfaceView playbackSurfaceView;
    private AspectRatioSurfaceView publishPreviewView;
    private EditText urlInput;
    private TextView statusView;
    private TextView metricsView;
    private Spinner streamSourceSpinner;
    private Spinner decodePathSpinner;
    private Spinner publishTransportSpinner;
    private EditText rtpRemoteHostInput;
    private EditText rtpRemotePortInput;
    private EditText rtpLocalPortInput;
    private Button startButton;
    private Button stopButton;
    private Button testPatternButton;
    private Button publishButton;
    private Button tcpTestButton;

    private volatile boolean statusPolling;
    private Thread statusPollingThread;
    private boolean onPublishPage;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeBridge = new NativeBridge();
        cameraPublisher = new AndroidCameraRtmpPublisher(this, nativeBridge, this);
        mediaPlayerBackend = new SystemMediaPlayerBackend(this);
        android.util.Log.i("StreamBridgeUI", "BuildInfo VERSION=" + BuildInfo.VERSION);
        showHomePage();
    }

    @Override
    protected void onDestroy() {
        stopAllSessions();
        mediaPlayerBackend.clearSurface();
        nativeBridge.release();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (cameraPublisher != null && cameraPublisher.isRunning()) {
            cameraPublisher.stop();
        }
        if (playbackSurfaceView != null || publishPreviewView != null) {
            stopPlayback();
            showHomePage();
            return;
        }
        super.onBackPressed();
    }

    private void showHomePage() {
        clearPageState();
        LinearLayout root = verticalRoot();

        TextView title = new TextView(this);
        title.setText("StreamBridge");
        title.setTextSize(24);
        root.addView(title, matchWrap());

        TextView version = new TextView(this);
        version.setText("Version: " + BuildInfo.VERSION);
        root.addView(version, matchWrap());

        Button playbackPageButton = new Button(this);
        playbackPageButton.setText("Playback Receiver");
        playbackPageButton.setOnClickListener(view -> showPlaybackPage());
        root.addView(playbackPageButton, matchWrap());

        Button publishPageButton = new Button(this);
        publishPageButton.setText("Android Publisher");
        publishPageButton.setOnClickListener(view -> showPublishPage());
        root.addView(publishPageButton, matchWrap());

        setScrollableContent(root);
    }

    private void showPlaybackPage() {
        stopAllSessions();
        clearPageState();
        onPublishPage = false;

        LinearLayout root = verticalRoot();
        root.addView(backButton(), matchWrap());

        playbackSurfaceView = new AspectRatioSurfaceView(this);
        playbackSurfaceView.setAspectRatio(16, 9);
        playbackSurfaceView.getHolder().addCallback(this);
        root.addView(playbackSurfaceView, matchWrap());

        urlInput = new EditText(this);
        urlInput.setSingleLine(true);
        urlInput.setText(DEFAULT_PLAY_URL);
        root.addView(urlInput, matchWrap());

        streamSourceSpinner = new Spinner(this);
        ArrayAdapter<String> streamAdapter = new ArrayAdapter<>(
                this, android.R.layout.simple_spinner_item, STREAM_SOURCE_LABELS);
        streamAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        streamSourceSpinner.setAdapter(streamAdapter);
        streamSourceSpinner.setSelection(4);
        streamSourceSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                applySelectedStreamSource();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        root.addView(streamSourceSpinner, matchWrap());

        decodePathSpinner = new Spinner(this);
        ArrayAdapter<String> pathAdapter = new ArrayAdapter<>(
                this,
                android.R.layout.simple_spinner_item,
                new String[] {
                        "AUTO",
                        "MEDIACODEC_AHB_GPU",
                        "MEDIACODEC_SURFACE",
                        "FFMPEG_SOFTWARE"
                });
        pathAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        decodePathSpinner.setAdapter(pathAdapter);
        decodePathSpinner.setSelection(0);
        root.addView(decodePathSpinner, matchWrap());

        LinearLayout row1 = horizontalRow();
        startButton = new Button(this);
        startButton.setText("Start Playback");
        startButton.setOnClickListener(view -> startPlayback());
        row1.addView(startButton, weightWrap());

        stopButton = new Button(this);
        stopButton.setText("Stop");
        stopButton.setOnClickListener(view -> stopPlayback());
        row1.addView(stopButton, weightWrap());

        testPatternButton = new Button(this);
        testPatternButton.setText("System Player");
        testPatternButton.setOnClickListener(view -> renderNativeTestPattern());
        row1.addView(testPatternButton, weightWrap());
        root.addView(row1, matchWrap());

        tcpTestButton = new Button(this);
        tcpTestButton.setText("TCP Test");
        tcpTestButton.setOnClickListener(view -> testTcpFromInput());
        root.addView(tcpTestButton, matchWrap());

        addStatusViews(root, "Idle " + BuildInfo.VERSION, "Metrics: --");
        setScrollableContent(root);
    }

    private void showPublishPage() {
        stopAllSessions();
        clearPageState();
        onPublishPage = true;

        LinearLayout root = verticalRoot();
        root.addView(backButton(), matchWrap());

        publishPreviewView = new AspectRatioSurfaceView(this);
        publishPreviewView.setMaxHeightPx(
                Math.max(1, getResources().getDisplayMetrics().heightPixels / 2));
        applyPublishPreviewAspect();
        root.addView(publishPreviewView, matchWrap());

        publishTransportSpinner = new Spinner(this);
        ArrayAdapter<String> publishTransportAdapter = new ArrayAdapter<>(
                this,
                android.R.layout.simple_spinner_item,
                new String[] {"RTMP/FLV", "RTP/UDP video"});
        publishTransportAdapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        publishTransportSpinner.setAdapter(publishTransportAdapter);
        publishTransportSpinner.setSelection(0);
        root.addView(publishTransportSpinner, matchWrap());

        urlInput = new EditText(this);
        urlInput.setSingleLine(true);
        urlInput.setText(DEFAULT_PUBLISH_URL);
        root.addView(urlInput, matchWrap());

        rtpRemoteHostInput = new EditText(this);
        rtpRemoteHostInput.setSingleLine(true);
        rtpRemoteHostInput.setText(DEFAULT_RTP_REMOTE_HOST);
        root.addView(rtpRemoteHostInput, matchWrap());

        rtpRemotePortInput = new EditText(this);
        rtpRemotePortInput.setSingleLine(true);
        rtpRemotePortInput.setText(String.valueOf(DEFAULT_RTP_REMOTE_PORT));
        root.addView(rtpRemotePortInput, matchWrap());

        rtpLocalPortInput = new EditText(this);
        rtpLocalPortInput.setSingleLine(true);
        rtpLocalPortInput.setText(String.valueOf(DEFAULT_RTP_LOCAL_PORT));
        root.addView(rtpLocalPortInput, matchWrap());

        LinearLayout row = horizontalRow();
        publishButton = new Button(this);
        publishButton.setText("Start Publish");
        publishButton.setOnClickListener(view -> toggleCameraPublish());
        row.addView(publishButton, weightWrap());

        tcpTestButton = new Button(this);
        tcpTestButton.setText("TCP Test");
        tcpTestButton.setOnClickListener(view -> testTcpFromInput());
        row.addView(tcpTestButton, weightWrap());
        root.addView(row, matchWrap());

        addStatusViews(root, "PublishIdle " + BuildInfo.VERSION, "Publish: --");
        setScrollableContent(root);
    }

    private void clearPageState() {
        stopStatusPolling();
        playbackSurfaceView = null;
        publishPreviewView = null;
        urlInput = null;
        statusView = null;
        metricsView = null;
        streamSourceSpinner = null;
        decodePathSpinner = null;
        publishTransportSpinner = null;
        rtpRemoteHostInput = null;
        rtpRemotePortInput = null;
        rtpLocalPortInput = null;
        startButton = null;
        stopButton = null;
        testPatternButton = null;
        publishButton = null;
        tcpTestButton = null;
        onPublishPage = false;
    }

    private LinearLayout verticalRoot() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        return root;
    }

    private void setScrollableContent(LinearLayout root) {
        ScrollView scrollView = new ScrollView(this);
        scrollView.setFillViewport(true);
        scrollView.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scrollView);
    }

    private LinearLayout horizontalRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        return row;
    }

    private Button backButton() {
        Button button = new Button(this);
        button.setText("??????");
        button.setOnClickListener(view -> {
            stopAllSessions();
            showHomePage();
        });
        return button;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams weightWrap() {
        return new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
    }

    private void addStatusViews(LinearLayout root, String status, String metrics) {
        statusView = new TextView(this);
        statusView.setText(status);
        root.addView(statusView, matchWrap());

        metricsView = new TextView(this);
        metricsView.setText(metrics);
        root.addView(metricsView, matchWrap());
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (playbackSurfaceView == null || holder != playbackSurfaceView.getHolder()) {
            return;
        }
        mediaPlayerBackend.setSurface(holder.getSurface());
        nativeBridge.onSurfaceChanged(holder.getSurface());
        mediaPlayerBackend.retryIfPending();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (playbackSurfaceView == null || holder != playbackSurfaceView.getHolder()) {
            return;
        }
        mediaPlayerBackend.setSurface(holder.getSurface());
        nativeBridge.onSurfaceChanged(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (playbackSurfaceView == null || holder != playbackSurfaceView.getHolder()) {
            return;
        }
        mediaPlayerBackend.clearSurface();
        nativeBridge.onSurfaceDestroyed();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQUEST_PUBLISH_PERMISSIONS) {
            return;
        }
        boolean allGranted = grantResults.length > 0;
        for (int result : grantResults) {
            allGranted = allGranted && result == PackageManager.PERMISSION_GRANTED;
        }
        if (allGranted) {
            toggleCameraPublish();
        } else {
            onError("Camera/audio permission denied");
        }
    }

    private void startPlayback() {
        applySelectedStreamSource();
        String url = currentUrl();
        if (url.isEmpty()) {
            onError("URL is empty");
            return;
        }
        if (playbackSurfaceView == null) {
            onError("Playback surface missing");
            return;
        }

        if (url.startsWith("rtmp://") || url.startsWith("rtmps://")) {
            setButtonsEnabled(false);
            bindActiveNetworkForRtmp();

            int result = nativeBridge.start(
                    url,
                    playbackSurfaceView.getHolder().getSurface(),
                    selectedDecodePath());
            if (result == 0) {
                onStatus("Native playback started: " + nativeBridge.status());
                pollNativeStatus();
            } else {
                onError("Native start failed: " + result + " " + nativeBridge.status());
                setButtonsEnabled(true);
            }
        } else if (isRtpUrl(url)) {
            int localPort = rtpLocalPortFromUrl(url);
            if (localPort <= 0) {
                onError("RTP URL must include a UDP port, for example rtp://0.0.0.0:5004");
                return;
            }
            setButtonsEnabled(false);
            int result = nativeBridge.startRtpVideoReceive(
                    localPort,
                    playbackSurfaceView.getHolder().getSurface(),
                    DEFAULT_RTP_PLAY_WIDTH,
                    DEFAULT_RTP_PLAY_HEIGHT,
                    DEFAULT_RTP_PLAY_FRAME_RATE,
                    selectedDecodePath());
            if (result == 0) {
                onStatus("RTP video playback started: " + nativeBridge.status());
                pollNativeStatus();
            } else {
                onError("RTP video start failed: " + result + " " + nativeBridge.status());
                setButtonsEnabled(true);
            }
        } else {
            setButtonsEnabled(false);
            mediaPlayerBackend.start(url);
            setButtonsEnabled(true);
        }
    }

    private void renderNativeTestPattern() {
        String url = currentUrl();
        if (url.isEmpty()) {
            onError("URL is empty");
            return;
        }
        setButtonsEnabled(false);
        mediaPlayerBackend.start(url);
        setButtonsEnabled(true);
    }

    private void stopPlayback() {
        stopStatusPolling();
        if (mediaPlayerBackend != null) {
            mediaPlayerBackend.stop();
        }
        nativeBridge.stop();
        if (statusView != null && !onPublishPage) {
            statusView.setText("Stopped");
        }
        if (metricsView != null && !onPublishPage) {
            metricsView.setText("Metrics: --");
        }
        setButtonsEnabled(true);
    }

    private void toggleCameraPublish() {
        if (cameraPublisher != null && cameraPublisher.isRunning()) {
            cameraPublisher.stop();
            if (publishButton != null) {
                publishButton.setText("Start Publish");
            }
            setButtonsEnabled(true);
            return;
        }

        boolean rtpPublish = isRtpPublishSelected();
        boolean cameraMissing = checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED;
        boolean audioMissing = !rtpPublish
                && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED;
        if (cameraMissing || audioMissing) {
            requestPermissions(rtpPublish
                            ? new String[] { Manifest.permission.CAMERA }
                            : new String[] {
                                    Manifest.permission.CAMERA,
                                    Manifest.permission.RECORD_AUDIO
                            },
                    REQUEST_PUBLISH_PERMISSIONS);
            return;
        }

        String url = currentUrl();
        if (!rtpPublish && url.isEmpty()) {
            onError("URL is empty");
            return;
        }
        if (publishPreviewView == null) {
            onError("Preview surface missing");
            return;
        }
        applyPublishPreviewAspect();
        Surface previewSurface = publishPreviewView.getHolder().getSurface();
        if (previewSurface == null || !previewSurface.isValid()) {
            onError("Preview surface is not ready");
            return;
        }
        Rect surfaceFrame = publishPreviewView.getHolder().getSurfaceFrame();
        android.util.Log.i("StreamBridgePublisher",
                "preview view size=" + publishPreviewView.getWidth() + "x"
                        + publishPreviewView.getHeight()
                        + " surfaceFrame=" + rectToString(surfaceFrame)
                        + " version=" + BuildInfo.VERSION);

        if (!rtpPublish) {
            bindActiveNetworkForRtmp();
        }
        stopPlayback();
        setButtonsEnabled(false);
        if (publishButton != null) {
            publishButton.setEnabled(true);
            publishButton.setText("Stop Publish");
        }
        if (rtpPublish) {
            String host = currentRtpRemoteHost();
            int remotePort = currentRtpRemotePort();
            int localPort = currentRtpLocalPort();
            if (host.isEmpty() || remotePort <= 0) {
                onError("RTP host/port is invalid");
                setButtonsEnabled(true);
                return;
            }
            cameraPublisher.startRtp(host, remotePort, localPort, previewSurface);
        } else {
            cameraPublisher.start(url, previewSurface);
        }
    }

    private void applyPublishPreviewAspect() {
        if (publishPreviewView == null || cameraPublisher == null) {
            return;
        }
        int[] aspect = cameraPublisher.previewAspectForCurrentCamera();
        publishPreviewView.setAspectRatio(aspect[0], aspect[1]);
        android.util.Log.i("StreamBridgePublisher",
                "preview aspect set to " + aspect[0] + "x" + aspect[1]
                        + " version=" + BuildInfo.VERSION);
    }

    private String rectToString(Rect rect) {
        if (rect == null) {
            return "null";
        }
        return rect.left + "," + rect.top + "-" + rect.right + "," + rect.bottom
                + " (" + rect.width() + "x" + rect.height() + ")";
    }

    private void stopAllSessions() {
        stopStatusPolling();
        if (cameraPublisher != null && cameraPublisher.isRunning()) {
            cameraPublisher.stop();
        }
        if (mediaPlayerBackend != null) {
            mediaPlayerBackend.stop();
        }
        if (nativeBridge != null) {
            nativeBridge.stop();
        }
    }

    private void pollNativeStatus() {
        stopStatusPolling();
        statusPolling = true;
        statusPollingThread = new Thread(() -> {
            while (statusPolling) {
                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    break;
                }
                if (!statusPolling) {
                    break;
                }
                String status = nativeBridge.status();
                runOnUiThread(() -> {
                    if (statusView != null) {
                        statusView.setText(status);
                    }
                    if (metricsView != null) {
                        metricsView.setText(formatMetricsLine(status));
                    }
                });
                if (status.contains("Error") || status.contains("Stopped")) {
                    statusPolling = false;
                    runOnUiThread(() -> {
                        setButtonsEnabled(true);
                        if (status.contains("Error")) {
                            Toast.makeText(MainActivity.this, status, Toast.LENGTH_LONG).show();
                        }
                    });
                    return;
                }
            }
        }, "StreamBridgeStatusPoll");
        statusPollingThread.start();
    }

    private void stopStatusPolling() {
        statusPolling = false;
        if (statusPollingThread != null) {
            statusPollingThread.interrupt();
            statusPollingThread = null;
        }
    }

    private void bindActiveNetworkForRtmp() {
        try {
            ConnectivityManager cm =
                    (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            Network activeNetwork = cm.getActiveNetwork();
            if (activeNetwork != null) {
                boolean bound = cm.bindProcessToNetwork(activeNetwork);
                android.util.Log.i("StreamBridgeUI", "bindProcessToNetwork: " + bound
                        + " network=" + activeNetwork);
            } else {
                android.util.Log.w("StreamBridgeUI", "No active network!");
            }
        } catch (Exception e) {
            android.util.Log.e("StreamBridgeUI", "Network bind failed: " + e.getMessage());
        }
    }

    @Override
    public void onStatus(String message) {
        runOnUiThread(() -> {
            if (statusView != null) {
                statusView.setText(message);
            }
            if (metricsView != null) {
                metricsView.setText(formatMetricsLine(message));
            }
        });
    }

    @Override
    public void onError(String message) {
        runOnUiThread(() -> {
            if (statusView != null) {
                statusView.setText(message);
            }
            if (metricsView != null) {
                metricsView.setText(onPublishPage ? "Publish: --" : "Metrics: --");
            }
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        });
    }

    @Override
    public void onPublishStatus(String message) {
        runOnUiThread(() -> {
            if (statusView != null) {
                statusView.setText(message);
            }
            if (metricsView != null) {
                metricsView.setText("Publish: " + cameraPublisher.status());
            }
        });
    }

    @Override
    public void onPublishError(String message) {
        runOnUiThread(() -> {
            if (statusView != null) {
                statusView.setText(message);
            }
            if (metricsView != null) {
                metricsView.setText("Publish: " + nativeBridge.publishStatus());
            }
            if (publishButton != null) {
                publishButton.setText("Start Publish");
            }
            setButtonsEnabled(true);
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        });
    }

    private void setButtonsEnabled(boolean enabled) {
        if (startButton != null) {
            startButton.setEnabled(enabled);
        }
        if (stopButton != null) {
            stopButton.setEnabled(enabled);
        }
        if (testPatternButton != null) {
            testPatternButton.setEnabled(enabled);
        }
        if (publishButton != null) {
            publishButton.setEnabled(enabled);
        }
        if (tcpTestButton != null) {
            tcpTestButton.setEnabled(enabled);
        }
    }

    private boolean isRtpPublishSelected() {
        return publishTransportSpinner != null
                && publishTransportSpinner.getSelectedItemPosition() == 1;
    }

    private String currentRtpRemoteHost() {
        if (rtpRemoteHostInput == null) {
            return "";
        }
        return rtpRemoteHostInput.getText().toString().trim();
    }

    private int currentRtpRemotePort() {
        return parsePort(rtpRemotePortInput, DEFAULT_RTP_REMOTE_PORT);
    }

    private int currentRtpLocalPort() {
        return parsePort(rtpLocalPortInput, DEFAULT_RTP_LOCAL_PORT);
    }

    private int parsePort(EditText input, int fallback) {
        if (input == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(input.getText().toString().trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private String currentUrl() {
        if (urlInput == null) {
            return "";
        }
        return urlInput.getText().toString().trim();
    }

    private void applySelectedStreamSource() {
        String path = selectedStreamPath();
        if (path == null || urlInput == null) {
            return;
        }
        urlInput.setText(replaceRtmpPath(currentUrl(), path));
        urlInput.setSelection(urlInput.getText().length());
    }


    private boolean isRtpUrl(String url) {
        return url != null && url.trim().startsWith("rtp://");
    }

    private int rtpLocalPortFromUrl(String url) {
        if (url == null) {
            return -1;
        }
        String trimmed = url.trim();
        if (!trimmed.startsWith("rtp://")) {
            return -1;
        }
        int colon = trimmed.lastIndexOf(':');
        if (colon < "rtp://".length() || colon + 1 >= trimmed.length()) {
            return -1;
        }
        int slash = trimmed.indexOf('/', colon + 1);
        String portText = slash >= 0 ? trimmed.substring(colon + 1, slash) : trimmed.substring(colon + 1);
        try {
            int port = Integer.parseInt(portText.trim());
            return port > 0 && port <= 65535 ? port : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private String selectedStreamPath() {
        if (streamSourceSpinner == null) {
            return null;
        }
        int position = streamSourceSpinner.getSelectedItemPosition();
        if (position < 0 || position >= STREAM_SOURCE_PATHS.length) {
            return null;
        }
        return STREAM_SOURCE_PATHS[position];
    }

    private String replaceRtmpPath(String url, String path) {
        String base = rtmpBase(url);
        if (base == null) {
            base = rtmpBase(DEFAULT_PLAY_URL);
        }
        if (base == null) {
            return DEFAULT_PLAY_URL;
        }
        return base + path;
    }

    private String rtmpBase(String url) {
        if (url == null) {
            return null;
        }
        String trimmed = url.trim();
        int schemeEnd;
        if (trimmed.startsWith("rtmp://")) {
            schemeEnd = "rtmp://".length();
        } else if (trimmed.startsWith("rtmps://")) {
            schemeEnd = "rtmps://".length();
        } else {
            return null;
        }

        int pathStart = trimmed.indexOf('/', schemeEnd);
        if (pathStart < 0) {
            return trimmed;
        }
        return trimmed.substring(0, pathStart);
    }

    private int selectedDecodePath() {
        if (decodePathSpinner == null) {
            return NativeBridge.PATH_AUTO;
        }
        switch (decodePathSpinner.getSelectedItemPosition()) {
            case 1:
                return NativeBridge.PATH_MEDIACODEC_AHB_GPU;
            case 2:
                return NativeBridge.PATH_MEDIACODEC_SURFACE;
            case 3:
                return NativeBridge.PATH_FFMPEG_SOFTWARE;
            case 0:
            default:
                return NativeBridge.PATH_AUTO;
        }
    }

    private String formatMetricsLine(String status) {
        String bitrate = metricValue(status, "bitrate");
        String vfps = metricValue(status, "vfps");
        String afps = metricValue(status, "afps");
        String delay = metricValue(status, "buf_delay");
        String avDiff = metricValue(status, "av_diff");
        String drop = metricValue(status, "drop");
        String render = metricValue(status, "render");

        return "Metrics: bitrate=" + valueOrDash(bitrate)
                + " vfps=" + valueOrDash(vfps)
                + " afps=" + valueOrDash(afps)
                + " delay=" + valueOrDash(delay)
                + " av=" + valueOrDash(avDiff)
                + " drop=" + valueOrDash(drop)
                + " render=" + valueOrDash(render);
    }

    private String valueOrDash(String value) {
        return value == null || value.isEmpty() ? "--" : value;
    }

    private String metricValue(String status, String key) {
        if (status == null || key == null) {
            return null;
        }
        String prefix = key + "=";
        int start = status.indexOf(prefix);
        if (start < 0) {
            return null;
        }
        start += prefix.length();
        int end = status.indexOf(' ', start);
        if (end < 0) {
            end = status.length();
        }
        return status.substring(start, end);
    }

    private void testTcpFromInput() {
        String url = currentUrl();
        new Thread(() -> {
            String result = testTcpConnect(url);
            android.util.Log.i("StreamBridgeUI", "TCP test result: " + result);
            runOnUiThread(() -> {
                if (statusView != null) {
                    statusView.setText("TCP: " + result);
                }
                Toast.makeText(MainActivity.this, result, Toast.LENGTH_LONG).show();
            });
        }, "StreamBridgeTcpTest").start();
    }

    private String testTcpConnect(String rtmpUrl) {
        String host = rtmpUrl;
        int port = 1935;
        try {
            if (host.startsWith("rtmp://")) {
                host = host.substring(7);
            }
            int slashIdx = host.indexOf('/');
            if (slashIdx > 0) {
                host = host.substring(0, slashIdx);
            }
            int colonIdx = host.lastIndexOf(':');
            if (colonIdx > 0) {
                port = Integer.parseInt(host.substring(colonIdx + 1));
                host = host.substring(0, colonIdx);
            }

            android.util.Log.i("StreamBridgeUI", "TCP connecting to "
                    + host + ":" + port + " ...");
            java.net.Socket socket = new java.net.Socket();
            socket.connect(new java.net.InetSocketAddress(host, port), 5000);
            java.net.InetAddress local = socket.getLocalAddress();
            android.util.Log.i("StreamBridgeUI", "TCP OK: local=" + local.getHostAddress()
                    + " remote=" + socket.getInetAddress().getHostAddress());
            socket.close();
            return "OK: " + host + ":" + port;
        } catch (Exception e) {
            android.util.Log.e("StreamBridgeUI", "TCP FAIL: "
                    + e.getClass().getSimpleName() + ": " + e.getMessage());
            return "FAIL [" + e.getClass().getSimpleName() + "]: " + e.getMessage();
        }
    }
}
