#include "playback_reconnect_controller.h"

namespace streambridge::android {

PlaybackReconnectController::PlaybackReconnectController(int max_attempts, int delay_ms)
    : max_attempts_(max_attempts)
    , delay_ms_(delay_ms) {}

bool PlaybackReconnectController::can_try() const {
    return attempt_ <= max_attempts_;
}

bool PlaybackReconnectController::exhausted() const {
    return attempt_ > max_attempts_;
}

bool PlaybackReconnectController::is_reconnecting() const {
    return attempt_ > 0;
}

void PlaybackReconnectController::reset_after_connected() {
    attempt_ = 0;
}

void PlaybackReconnectController::record_failure() {
    ++attempt_;
}

}  // namespace streambridge::android
