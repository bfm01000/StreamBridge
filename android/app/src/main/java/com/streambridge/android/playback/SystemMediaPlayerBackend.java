package com.streambridge.android.playback;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.view.Surface;

import java.io.IOException;

public final class SystemMediaPlayerBackend {
    private final PlaybackEvents events;
    private MediaPlayer mediaPlayer;
    private Surface surface;
    private String pendingUrl;

    public SystemMediaPlayerBackend(PlaybackEvents events) {
        this.events = events;
    }

    public void setSurface(Surface newSurface) {
        surface = newSurface;
        if (mediaPlayer != null) {
            mediaPlayer.setSurface(surface);
        }
    }

    public void clearSurface() {
        surface = null;
        if (mediaPlayer != null) {
            mediaPlayer.setSurface(null);
        }
    }

    public void start(String url) {
        pendingUrl = url;
        stop();

        if (url == null || url.trim().isEmpty()) {
            events.onError("URL is empty");
            return;
        }
        if (surface == null || !surface.isValid()) {
            events.onError("Surface is not ready");
            return;
        }

        mediaPlayer = new MediaPlayer();
        mediaPlayer.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                .build());
        mediaPlayer.setSurface(surface);
        mediaPlayer.setOnPreparedListener(player -> {
            events.onStatus("Prepared, playing");
            player.start();
        });
        mediaPlayer.setOnVideoSizeChangedListener((player, width, height) ->
                events.onStatus("Video size: " + width + "x" + height));
        mediaPlayer.setOnCompletionListener(player -> events.onStatus("Playback completed"));
        mediaPlayer.setOnBufferingUpdateListener((player, percent) ->
                events.onStatus("Buffering " + percent + "%"));
        mediaPlayer.setOnErrorListener((player, what, extra) -> {
            events.onError("MediaPlayer error what=" + what + " extra=" + extra);
            stop();
            return true;
        });

        try {
            events.onStatus("Preparing: " + url);
            mediaPlayer.setDataSource(url);
            mediaPlayer.prepareAsync();
        } catch (IOException | IllegalArgumentException | SecurityException | IllegalStateException ex) {
            events.onError("Prepare failed: " + ex.getMessage());
            stop();
        }
    }

    public void retryIfPending() {
        if (pendingUrl != null && mediaPlayer == null) {
            start(pendingUrl);
        }
    }

    public void stop() {
        if (mediaPlayer == null) {
            return;
        }

        try {
            mediaPlayer.setOnPreparedListener(null);
            mediaPlayer.setOnVideoSizeChangedListener(null);
            mediaPlayer.setOnCompletionListener(null);
            mediaPlayer.setOnBufferingUpdateListener(null);
            mediaPlayer.setOnErrorListener(null);
            if (mediaPlayer.isPlaying()) {
                mediaPlayer.stop();
            }
        } catch (IllegalStateException ignored) {
            // MediaPlayer state may already be terminal during error callbacks.
        } finally {
            mediaPlayer.release();
            mediaPlayer = null;
            events.onStatus("Stopped");
        }
    }
}
