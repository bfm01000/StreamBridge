package com.streambridge.android;

import android.app.Activity;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.ViewGroup;
import android.view.View;
import android.widget.AdapterView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.TextView;
import android.widget.Toast;

public final class MainActivity extends Activity implements SurfaceHolder.Callback, PlaybackEvents {
    private static final String DEFAULT_RTMP_URL = "rtmp://192.168.31.57:1935/live/test";
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
    private SystemMediaPlayerBackend mediaPlayerBackend;
    private AspectRatioSurfaceView surfaceView;
    private EditText urlInput;
    private TextView statusView;
    private TextView metricsView;
    private Spinner streamSourceSpinner;
    private Spinner decodePathSpinner;
    private Button startButton;
    private Button stopButton;
    private Button testPatternButton;
    private Button tcpTestButton;
    private volatile boolean statusPolling;
    private Thread statusPollingThread;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeBridge = new NativeBridge();
        mediaPlayerBackend = new SystemMediaPlayerBackend(this);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        surfaceView = new AspectRatioSurfaceView(this);
        surfaceView.setAspectRatio(16, 9);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        urlInput = new EditText(this);
        urlInput.setSingleLine(true);
        urlInput.setText(DEFAULT_RTMP_URL);
        root.addView(urlInput, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        streamSourceSpinner = new Spinner(this);
        ArrayAdapter<String> streamAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                STREAM_SOURCE_LABELS);
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
        root.addView(streamSourceSpinner, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        decodePathSpinner = new Spinner(this);
        ArrayAdapter<String> pathAdapter = new ArrayAdapter<>(this,
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
        root.addView(decodePathSpinner, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // 第一行按钮：播放 / 停止 / HTTP备选
        LinearLayout row1 = new LinearLayout(this);
        row1.setOrientation(LinearLayout.HORIZONTAL);

        startButton = new Button(this);
        startButton.setText("RTMP播放");
        startButton.setOnClickListener(view -> startPlayback());
        row1.addView(startButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        stopButton = new Button(this);
        stopButton.setText("停止");
        stopButton.setOnClickListener(view -> stopPlayback());
        row1.addView(stopButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        testPatternButton = new Button(this);
        testPatternButton.setText("HTTP备选");
        testPatternButton.setOnClickListener(view -> renderNativeTestPattern());
        row1.addView(testPatternButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        root.addView(row1, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // 第二行按钮：TCP连通性测试
        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);

        tcpTestButton = new Button(this);
        tcpTestButton.setText("TCP测试");
        tcpTestButton.setOnClickListener(view -> testTcpFromInput());
        row2.addView(tcpTestButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        root.addView(row2, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        statusView = new TextView(this);
        statusView.setText("Idle");
        root.addView(statusView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        metricsView = new TextView(this);
        metricsView.setText("Metrics: --");
        root.addView(metricsView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
    }

    @Override
    protected void onDestroy() {
        stopPlayback();
        mediaPlayerBackend.clearSurface();
        nativeBridge.release();
        super.onDestroy();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        mediaPlayerBackend.setSurface(holder.getSurface());
        nativeBridge.onSurfaceChanged(holder.getSurface());
        mediaPlayerBackend.retryIfPending();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        mediaPlayerBackend.setSurface(holder.getSurface());
        nativeBridge.onSurfaceChanged(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        mediaPlayerBackend.clearSurface();
        nativeBridge.onSurfaceDestroyed();
    }

    private void startPlayback() {
        applySelectedStreamSource();
        String url = urlInput.getText().toString().trim();
        if (url.isEmpty()) {
            onError("URL is empty");
            return;
        }
        // RTMP 走 native FFmpeg 后端
        if (url.startsWith("rtmp://") || url.startsWith("rtmps://")) {
            setButtonsEnabled(false);

            // Android 6.0+ 原生 POSIX socket 需要绑定到当前活跃网络（WiFi/蜂窝）
            // 否则 connect() 会报 EADDRNOTAVAIL
            try {
                ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
                Network activeNetwork = cm.getActiveNetwork();
                if (activeNetwork != null) {
                    boolean bound = cm.bindProcessToNetwork(activeNetwork);
                    android.util.Log.i("StreamBridgeUI", "bindProcessToNetwork: " + bound +
                            " network=" + activeNetwork);
                } else {
                    android.util.Log.w("StreamBridgeUI", "No active network!");
                }
            } catch (Exception e) {
                android.util.Log.e("StreamBridgeUI", "Network bind failed: " + e.getMessage());
            }

            int result = nativeBridge.start(url, surfaceView.getHolder().getSurface(),
                    selectedDecodePath());
            if (result == 0) {
                onStatus("Native playback started: " + nativeBridge.status());
                pollNativeStatus();
            } else {
                onError("Native start failed: " + result + " " + nativeBridge.status());
                setButtonsEnabled(true);
            }
        } else {
            // HTTP/HTTPS/本地文件走系统 MediaPlayer
            setButtonsEnabled(false);
            mediaPlayerBackend.start(url);
            setButtonsEnabled(true);
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
                    statusView.setText(status);
                    metricsView.setText(formatMetricsLine(status));
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

    private void renderNativeTestPattern() {
        // 保留：系统 MediaPlayer 播放非 RTMP 地址
        String url = urlInput.getText().toString().trim();
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
        if (statusView != null) {
            statusView.setText("Stopped");
        }
        if (metricsView != null) {
            metricsView.setText("Metrics: --");
        }
        setButtonsEnabled(true);
    }

    @Override
    public void onStatus(String message) {
        runOnUiThread(() -> {
            statusView.setText(message);
            if (metricsView != null) {
                metricsView.setText(formatMetricsLine(message));
            }
        });
    }

    @Override
    public void onError(String message) {
        runOnUiThread(() -> {
            statusView.setText(message);
            if (metricsView != null) {
                metricsView.setText("Metrics: --");
            }
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        });
    }

    private void setButtonsEnabled(boolean enabled) {
        startButton.setEnabled(enabled);
        stopButton.setEnabled(enabled);
        testPatternButton.setEnabled(enabled);
    }

    private void applySelectedStreamSource() {
        String path = selectedStreamPath();
        if (path == null) {
            return;
        }
        String currentUrl = urlInput.getText().toString().trim();
        urlInput.setText(replaceRtmpPath(currentUrl, path));
        urlInput.setSelection(urlInput.getText().length());
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
            base = rtmpBase(DEFAULT_RTMP_URL);
        }
        if (base == null) {
            return DEFAULT_RTMP_URL;
        }
        return base + path;
    }

    private String rtmpBase(String url) {
        if (url == null) {
            return null;
        }
        String trimmed = url.trim();
        int schemeEnd = -1;
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

        return "Metrics: bitrate=" + valueOrDash(bitrate) +
                " vfps=" + valueOrDash(vfps) +
                " afps=" + valueOrDash(afps) +
                " delay=" + valueOrDash(delay) +
                " av=" + valueOrDash(avDiff) +
                " drop=" + valueOrDash(drop) +
                " render=" + valueOrDash(render);
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
        String url = urlInput.getText().toString().trim();
        new Thread(() -> {
            String result = testTcpConnect(url);
            android.util.Log.i("StreamBridgeUI", "TCP test result: " + result);
            runOnUiThread(() -> {
                statusView.setText("TCP: " + result);
                Toast.makeText(MainActivity.this, result, Toast.LENGTH_LONG).show();
            });
        }).start();
    }

    private String testTcpConnect(String rtmpUrl) {
        // 从 rtmp://host:port/path 中提取 host 和 port
        String host = rtmpUrl;
        int port = 1935;
        try {
            if (host.startsWith("rtmp://")) host = host.substring(7);
            int slashIdx = host.indexOf('/');
            if (slashIdx > 0) host = host.substring(0, slashIdx);
            int colonIdx = host.lastIndexOf(':');
            if (colonIdx > 0) {
                port = Integer.parseInt(host.substring(colonIdx + 1));
                host = host.substring(0, colonIdx);
            }

            android.util.Log.i("StreamBridgeUI", "TCP connecting to " + host + ":" + port + " ...");
            java.net.Socket s = new java.net.Socket();
            s.connect(new java.net.InetSocketAddress(host, port), 5000);
            java.net.InetAddress local = s.getLocalAddress();
            android.util.Log.i("StreamBridgeUI", "TCP OK: local=" + local.getHostAddress() +
                    " remote=" + s.getInetAddress().getHostAddress());
            s.close();
            return "OK: " + host + ":" + port;
        } catch (Exception e) {
            android.util.Log.e("StreamBridgeUI", "TCP FAIL: " + e.getClass().getSimpleName() +
                    ": " + e.getMessage());
            return "FAIL [" + e.getClass().getSimpleName() + "]: " + e.getMessage();
        }
    }
}
