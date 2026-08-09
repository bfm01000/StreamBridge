package com.streambridge.android;

import android.app.Activity;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

public final class MainActivity extends Activity implements SurfaceHolder.Callback, PlaybackEvents {
    private NativeBridge nativeBridge;
    private SystemMediaPlayerBackend mediaPlayerBackend;
    private SurfaceView surfaceView;
    private EditText urlInput;
    private TextView statusView;
    private Button startButton;
    private Button stopButton;
    private Button testPatternButton;
    private Button tcpTestButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        nativeBridge = new NativeBridge();
        mediaPlayerBackend = new SystemMediaPlayerBackend(this);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f));

        urlInput = new EditText(this);
        urlInput.setSingleLine(true);
        urlInput.setText("rtmp://192.168.31.57:1935/live/test");
        root.addView(urlInput, new LinearLayout.LayoutParams(
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
        String url = urlInput.getText().toString().trim();
        if (url.isEmpty()) {
            onError("URL is empty");
            return;
        }
        // RTMP 走 native FFmpeg 后端
        if (url.startsWith("rtmp://") || url.startsWith("rtmps://")) {
            setButtonsEnabled(false);
            int result = nativeBridge.start(url, surfaceView.getHolder().getSurface());
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
        new Thread(() -> {
            for (int i = 0; i < 600; i++) {  // poll up to 60s
                try { Thread.sleep(100); } catch (InterruptedException e) { break; }
                String status = nativeBridge.status();
                runOnUiThread(() -> statusView.setText(status));
                if (status.contains("Error") || status.contains("Stopped")) {
                    runOnUiThread(() -> {
                        setButtonsEnabled(true);
                        if (status.contains("Error")) {
                            Toast.makeText(MainActivity.this, status, Toast.LENGTH_LONG).show();
                        }
                    });
                    return;
                }
            }
        }).start();
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
        if (mediaPlayerBackend != null) {
            mediaPlayerBackend.stop();
        }
        nativeBridge.stop();
        if (statusView != null) {
            statusView.setText("Stopped");
        }
        setButtonsEnabled(true);
    }

    @Override
    public void onStatus(String message) {
        runOnUiThread(() -> statusView.setText(message));
    }

    @Override
    public void onError(String message) {
        runOnUiThread(() -> {
            statusView.setText(message);
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        });
    }

    private void setButtonsEnabled(boolean enabled) {
        startButton.setEnabled(enabled);
        stopButton.setEnabled(enabled);
        testPatternButton.setEnabled(enabled);
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
