#pragma once
// 设备源双路起始对齐（纯逻辑，便于单元测试）
//
// 背景：V4L2/ALSA 两个采集线程的启动时刻不同，各自首包携带绝对单调时钟 PTS。
// 若直接按绝对 PTS 交织，播放端会出现等于「启动时间差」的恒定音画偏移；
// 文件源两路共享同一 demux 时间线、天然对齐，设备源则必须显式对齐。
//
// 策略：
//   1. 对齐前（双路首包未齐）：两路都「只等待、不丢弃、不写出」。
//      若在单路有包时立即丢弃，两路包会因 33ms/21ms 到达网格不同步而几乎
//      永远不同时出现在队列里，对齐被无限推迟。
//   2. 双路首包齐备：base = max(视频首包 PTS, 音频首包 PTS)，即以「较晚启动
//      的一路」为零点；此后早于 base 的包（较早启动那一路的启动期数据）
//      全部丢弃——那些包没有对应的对端媒体，属无效数据。
//   3. 对齐后所有包平移 pts-base，两路都从 0 开始，与文件源行为一致；
//      两路 PTS 都来自单调时钟，任意一路丢帧/XRUN 都不会再引入漂移。
//
// 超时保护：对端流长时间不产生包（如音频设备故障）时，以当前可见流的
// 首包为基准强制对齐，避免无限等待导致无输出。

#include <cstdint>

namespace streambridge {

class AvStartAligner {
public:
    // 对齐超时：3 秒内对端流没有任何包，则强制对齐
    static constexpr int64_t kAlignTimeoutUs = 3'000'000;

    enum class Action {
        Wait,          // 对齐前：等待（不消费任何包，让首包留在队列里）
        DropVideo,     // 对齐后：该视频包早于 base，丢弃
        DropAudio,     // 对齐后：该音频包早于 base，丢弃
        Passthrough,   // 正常写出（已对齐且不低于 base）
    };

    struct Decision {
        Action action = Action::Wait;
        bool just_aligned = false;  // 本次调用建立了 base（供调用方记录日志）
    };

    // 每轮 mux 交织前调用。
    // have_video/have_audio: 两路队列当前是否可 peek 到队首包；
    // video_pts_us/audio_pts_us: 对应队首包的 PTS（微秒，单调时钟域）；
    // now_us: 当前单调时钟（用于对齐超时保护）。
    Decision on_peek(bool have_video, int64_t video_pts_us,
                     bool have_audio, int64_t audio_pts_us,
                     int64_t now_us) {
        if (!aligned_) {
            if (have_video || have_audio) {
                if (first_seen_us_ < 0) {
                    first_seen_us_ = now_us;
                } else if (now_us - first_seen_us_ > kAlignTimeoutUs) {
                    // 超时保护：以当前可见流为准强制对齐
                    base_us_ = have_video ? video_pts_us : audio_pts_us;
                    aligned_ = true;
                    return {Action::Passthrough, true};
                }
            }
            if (have_video && have_audio) {
                // 双路首包齐备：较晚启动的一路为零点
                base_us_ = video_pts_us > audio_pts_us ? video_pts_us : audio_pts_us;
                aligned_ = true;
                return {Action::Passthrough, true};
            }
            // 只等不丢：过早丢弃会导致两路包永远不同时在队，对齐饿死
            return {Action::Wait, false};
        }

        // 已对齐：早于 base 的包继续丢弃（较早启动那一路的启动期数据）
        if (have_video && video_pts_us < base_us_) return {Action::DropVideo, false};
        if (have_audio && audio_pts_us < base_us_) return {Action::DropAudio, false};
        return {Action::Passthrough, false};
    }

    bool aligned() const { return aligned_; }
    int64_t base_us() const { return base_us_; }

    // 对齐后：绝对 PTS 平移到零点。未对齐时恒等（base=0），调用无害。
    int64_t adjust(int64_t pts_us) const { return pts_us - base_us_; }

    void reset() {
        aligned_ = false;
        base_us_ = 0;
        first_seen_us_ = -1;
    }

private:
    bool aligned_ = false;
    int64_t base_us_ = 0;
    int64_t first_seen_us_ = -1;
};

}  // namespace streambridge
