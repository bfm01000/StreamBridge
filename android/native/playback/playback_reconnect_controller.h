#pragma once

namespace streambridge::android {

// 播放断线重连控制器：按最大尝试次数与延迟策略决定是否允许重试，并维护重连状态。
class PlaybackReconnectController {
public:
    PlaybackReconnectController(int max_attempts, int delay_ms);

    bool can_try() const;
    bool exhausted() const;
    bool is_reconnecting() const;

    int attempt() const { return attempt_; }
    int max_attempts() const { return max_attempts_; }
    int delay_ms() const { return delay_ms_; }

    void reset_after_connected();
    void record_failure();

private:
    int max_attempts_ = 0;
    int delay_ms_ = 0;
    int attempt_ = 0;
};

}  // namespace streambridge::android
