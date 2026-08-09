package com.streambridge.android;

interface PlaybackEvents {
    void onStatus(String message);

    void onError(String message);
}
