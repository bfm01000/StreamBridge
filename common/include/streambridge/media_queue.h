#pragma once
// 有界线程安全队列 — 所有跨线程媒体数据传递的通道

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include "streambridge/media_types.h"

namespace streambridge {

// ============================================================
// Push/Pop 操作结果
// ============================================================
enum class QueueResult {
    Ok,
    Timeout,
    Aborted,
    Full,
    Empty,
};

inline const char* queue_result_name(QueueResult r) {
    switch (r) {
        case QueueResult::Ok: return "Ok";
        case QueueResult::Timeout: return "Timeout";
        case QueueResult::Aborted: return "Aborted";
        case QueueResult::Full: return "Full";
        case QueueResult::Empty: return "Empty";
        default: return "?";
    }
}

// ============================================================
// 队列统计
// ============================================================
struct QueueStats {
    size_t total_pushed = 0;
    size_t total_popped = 0;
    size_t total_dropped = 0;       // 因满而丢弃
    size_t max_observed_size = 0;
    int64_t total_wait_us = 0;      // pop 累计等待时间
    int64_t max_wait_us = 0;        // 单次最长等待
};

// ============================================================
// MediaQueue<T>
// ============================================================

template<typename T>
class MediaQueue {
public:
    // 容量模式
    enum class CapacityMode {
        ByCount,     // 按元素数量限制
        ByDuration,  // 按时间跨度限制（元素需有 duration 或 pts 字段）
    };

    struct Config {
        size_t max_elements = 30;
        TimeDeltaUs max_duration{2'000'000};  // 2 秒
        CapacityMode mode = CapacityMode::ByCount;
        bool drop_oldest_on_full = true;
        TimeDeltaUs push_timeout{100'000};    // 100ms
        TimeDeltaUs pop_timeout{5'000'000};   // 5s
    };

    explicit MediaQueue(const Config& config = Config{}) : config_(config) {}
    ~MediaQueue() { shutdown(); }

    // 禁止拷贝
    MediaQueue(const MediaQueue&) = delete;
    MediaQueue& operator=(const MediaQueue&) = delete;

    // === 写入端 ===

    QueueResult push(T item, TimeDeltaUs timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::microseconds(timeout.us);

        while (!aborted_) {
            if (!is_full_unlocked()) {
                do_push_unlocked(std::move(item));
                return QueueResult::Ok;
            }
            if (config_.mode == CapacityMode::ByCount && config_.drop_oldest_on_full) {
                do_drop_oldest_unlocked();
                do_push_unlocked(std::move(item));
                return QueueResult::Ok;
            }
            // 等待空间
            if (cv_not_full_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return QueueResult::Timeout;
            }
        }
        return QueueResult::Aborted;
    }

    QueueResult push(T item) {
        return push(std::move(item), config_.push_timeout);
    }

    QueueResult try_push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_) return QueueResult::Aborted;
        if (!is_full_unlocked()) {
            do_push_unlocked(std::move(item));
            return QueueResult::Ok;
        }
        return QueueResult::Full;
    }

    // === 读取端 ===

    QueueResult pop(T& item, TimeDeltaUs timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::microseconds(timeout.us);
        auto wait_start = std::chrono::steady_clock::now();

        while (!aborted_) {
            if (!queue_.empty()) {
                do_pop_unlocked(item);
                auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - wait_start).count();
                stats_.total_wait_us += waited;
                if (waited > stats_.max_wait_us) stats_.max_wait_us = waited;
                return QueueResult::Ok;
            }
            if (cv_not_empty_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return QueueResult::Timeout;
            }
        }
        return QueueResult::Aborted;
    }

    QueueResult pop(T& item) {
        return pop(item, config_.pop_timeout);
    }

    QueueResult try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_) return QueueResult::Aborted;
        if (queue_.empty()) return QueueResult::Empty;
        do_pop_unlocked(item);
        return QueueResult::Ok;
    }

    // try_peek: 查看队首但不取出（用于音视频交织时比较 PTS）
    QueueResult try_peek(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_) return QueueResult::Aborted;
        if (queue_.empty()) return QueueResult::Empty;
        item = queue_.front();
        return QueueResult::Ok;
    }

    // === 控制 ===

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        cv_not_full_.notify_all();
        // 注意：flush 不清除 aborted 标志
    }

    void abort() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
        }
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    void shutdown() {
        abort();
        // shutdown 后不可恢复，清空内存
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = false;
        queue_.clear();
    }

    // === 水位 ===

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    TimeDeltaUs duration_us() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return duration_unlocked();
    }

    size_t max_size() const { return config_.max_elements; }

    bool is_aborted() const { return aborted_.load(std::memory_order_acquire); }

    // === 统计 ===

    QueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    // --- 内部方法（调用时持有 mutex）---

    bool is_full_unlocked() const {
        if (config_.mode == CapacityMode::ByCount) {
            return queue_.size() >= config_.max_elements;
        } else {
            return duration_unlocked() >= config_.max_duration;
        }
    }

    TimeDeltaUs duration_unlocked() const {
        if (queue_.size() < 2) return TimeDeltaUs::zero();
        // 使用元素的 pts 计算跨度
        TimePointUs first = peek_first_pts_unlocked();
        TimePointUs last = peek_last_pts_unlocked();
        return last - first;
    }

    // 获取元素的 pts（用于 ByDuration 模式）
    // 使用 SFINAE 检测 T 是否有 pts 成员
    template<typename U = T>
    typename std::enable_if<true, TimePointUs>::type
    peek_first_pts_unlocked() const {
        return queue_.front().pts;
    }

    template<typename U = T>
    typename std::enable_if<true, TimePointUs>::type
    peek_last_pts_unlocked() const {
        return queue_.back().pts;
    }

    void do_push_unlocked(T item) {
        queue_.push_back(std::move(item));
        stats_.total_pushed++;
        if (queue_.size() > stats_.max_observed_size) {
            stats_.max_observed_size = queue_.size();
        }
        cv_not_empty_.notify_one();
    }

    void do_pop_unlocked(T& item) {
        item = std::move(queue_.front());
        queue_.pop_front();
        stats_.total_popped++;
        cv_not_full_.notify_one();
    }

    void do_drop_oldest_unlocked() {
        if (queue_.empty()) return;
        queue_.pop_front();
        stats_.total_dropped++;
    }

    // --- 成员 ---
    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<T> queue_;
    std::atomic<bool> aborted_{false};
    QueueStats stats_;
};

}  // namespace streambridge
