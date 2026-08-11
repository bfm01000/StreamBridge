#pragma once
// 轻量级取消令牌，用于中断阻塞的 FFmpeg 调用

#include <atomic>
#include <memory>

namespace streambridge {

// 取消令牌：查询停止请求，供 FFmpeg interrupt 回调轮询
class StopToken {
public:
    StopToken() : flag_(nullptr) {}
    explicit StopToken(std::shared_ptr<std::atomic<bool>> flag)
        : flag_(std::move(flag)) {}

    bool stop_requested() const {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

    // FFmpeg interrupt callback 用
    // int cb(void* opaque) {
    //   auto* tok = static_cast<StopToken*>(opaque);
    //   return tok->stop_requested() ? 1 : 0;
    // }
    void* as_opaque() { return static_cast<void*>(this); }
    const void* as_opaque() const { return static_cast<const void*>(this); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// 取消源：请求停止并派发令牌，支持重置用于重连
class StopSource {
public:
    StopSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void request_stop() {
        flag_->store(true, std::memory_order_release);
    }

    StopToken token() const {
        return StopToken(flag_);
    }

    bool stop_requested() const {
        return flag_->load(std::memory_order_acquire);
    }

    // 重置（用于重连）
    void reset() {
        flag_->store(false, std::memory_order_release);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace streambridge
