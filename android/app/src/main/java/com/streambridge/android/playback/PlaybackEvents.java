package com.streambridge.android.playback;

public interface PlaybackEvents {
    void onStatus(String message);

    void onError(String message);
}
