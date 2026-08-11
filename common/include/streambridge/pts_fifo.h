#pragma once
// PTS FIFO queue — single-writer, single-reader
// Used by decoders to track packet PTS → output frame PTS ordering

#include <cstdint>
#include <deque>

namespace streambridge {

// PTS 先进先出队列：单写单读，跟踪输入包 PTS 与输出帧的对应关系
class PtsFifo {
public:
    // Push PTS in microseconds (called before avcodec_send_packet)
    void push(int64_t pts_us) { queue_.push_back(pts_us); }

    // Pop the oldest PTS (called after avcodec_receive_frame produces a frame)
    // Returns -1 if queue is empty
    int64_t pop() {
        if (queue_.empty()) return -1;
        int64_t v = queue_.front();
        queue_.pop_front();
        return v;
    }

    // Undo last push (called when avcodec_send_packet fails)
    void pop_back() {
        if (!queue_.empty()) queue_.pop_back();
    }

    // Peek front without popping. Returns -1 if empty.
    int64_t front() const {
        if (queue_.empty()) return -1;
        return queue_.front();
    }

    void clear() { queue_.clear(); }
    size_t size() const { return queue_.size(); }
    bool empty() const { return queue_.empty(); }

private:
    std::deque<int64_t> queue_;
};

}  // namespace streambridge
