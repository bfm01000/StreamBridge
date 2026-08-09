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

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);

        startButton = new Button(this);
        startButton.setText("Start");
        startButton.setOnClickListener(view -> startPlayback());
        buttons.addView(startButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        stopButton = new Button(this);
        stopButton.setText("Stop");
        stopButton.setOnClickListener(view -> stopPlayback());
        buttons.addView(stopButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        testPatternButton = new Button(this);
        testPatternButton.setText("Native Test");
        testPatternButton.setOnClickListener(view -> renderNativeTestPattern());
        buttons.addView(testPatternButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        root.addView(buttons, new LinearLayout.LayoutParams(
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
        setButtonsEnabled(false);
        mediaPlayerBackend.start(url);
        setButtonsEnabled(true);
    }

    private void renderNativeTestPattern() {
        String url = urlInput.getText().toString().trim();
        int result = nativeBridge.start(url, surfaceView.getHolder().getSurface());
        if (result == 0) {
            onStatus("Native test pattern rendered: " + nativeBridge.status());
        } else {
            onError("Native start failed: " + result + " " + nativeBridge.status());
        }
    }

    private void stopPlayback() {
        if (mediaPlayerBackend != null) {
            mediaPlayerBackend.stop();
        }
        nativeBridge.stop();
        if (statusView != null) {
            statusView.setText("Stopped");
        }
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
}
