// AvStartAligner 单元测试 — 设备源双路起始对齐纯逻辑
// 无任何设备/FFmpeg 依赖，可在无摄像头、无声卡的机器上运行。
//
// 覆盖：
//   1. 视频先启动：对齐前只等待不丢弃，双路首包齐备后 base=max(v_first,a_first)
//   2. 音频先启动（异常但合法）
//   3. 两路首包同时可见
//   4. 对齐后仍早于 base 的包继续丢弃
//   5. 对齐超时保护：3s 对端无包 → 以当前可见流强制对齐
//   6. 超时前不强制对齐（继续等待）
//   7. reset 恢复初始状态

#include "streambridge/av_start_aligner.h"

#include <cstdio>

using namespace streambridge;

static int g_failures = 0;

#define CHECK(cond, msg)                                      \
    do {                                                      \
        if (cond) {                                           \
            printf("  PASS: %s\n", msg);                      \
        } else {                                              \
            printf("  FAIL: %s\n", msg);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

int main() {
    printf("== AvStartAligner unit tests ==\n");

    // 1. 视频先到：对齐前只等待（不丢包），双路首包齐备后 base = max(两路首包)
    {
        printf("[1] video-first normal alignment\n");
        AvStartAligner al;
        auto d = al.on_peek(true, 1'000'000, false, 0, 0);
        CHECK(d.action == AvStartAligner::Action::Wait,
              "video-only pre-align -> Wait (not drop!)");
        CHECK(!al.aligned(), "not aligned while audio absent");

        d = al.on_peek(true, 1'000'000, true, 1'400'000, 400'000);
        CHECK(d.just_aligned && al.aligned(), "both first packets -> aligned");
        CHECK(al.base_us() == 1'400'000, "base = max(v_first, a_first)");

        // 对齐后：视频首包(1e6) < base(1.4e6) -> 丢弃；音频首包=base -> 保留
        d = al.on_peek(true, 1'000'000, true, 1'400'000, 401'000);
        CHECK(d.action == AvStartAligner::Action::DropVideo,
              "video packet below base -> dropped after align");
        d = al.on_peek(true, 1'500'000, true, 1'400'000, 500'000);
        CHECK(d.action == AvStartAligner::Action::Passthrough,
              "video at/above base -> passthrough");
        CHECK(al.adjust(1'500'000) == 100'000, "adjust subtracts base");
        CHECK(al.adjust(1'400'000) == 0, "packet at base adjusts to 0");
    }

    // 2. 音频先到
    {
        printf("[2] audio-first alignment\n");
        AvStartAligner al;
        auto d = al.on_peek(false, 0, true, 900'000, 0);
        CHECK(d.action == AvStartAligner::Action::Wait,
              "audio-only pre-align -> Wait");
        d = al.on_peek(true, 1'000'000, true, 900'000, 100'000);
        CHECK(d.just_aligned && al.base_us() == 1'000'000,
              "base = max = video first");
        d = al.on_peek(true, 1'000'000, true, 900'000, 101'000);
        CHECK(d.action == AvStartAligner::Action::DropAudio,
              "audio below base -> dropped");
    }

    // 3. 两路首包同时可见
    {
        printf("[3] simultaneous first packets\n");
        AvStartAligner al;
        auto d = al.on_peek(true, 2'000'000, true, 2'000'500, 0);
        CHECK(d.just_aligned && al.base_us() == 2'000'500, "base = max of firsts");
        d = al.on_peek(true, 2'000'000, true, 2'000'500, 1000);
        CHECK(d.action == AvStartAligner::Action::DropVideo,
              "below-base video dropped right after align");
    }

    // 4. 对齐后低于 base 的音频包丢弃
    {
        printf("[4] post-align below-base audio dropped\n");
        AvStartAligner al;
        al.on_peek(true, 1'000'000, true, 1'300'000, 0);
        auto d = al.on_peek(true, 1'500'000, true, 1'200'000, 200'000);
        CHECK(d.action == AvStartAligner::Action::DropAudio,
              "below-base audio -> DropAudio");
    }

    // 5. 超时保护：3s 内对端无包 -> 以当前可见流强制对齐
    {
        printf("[5] timeout force-align\n");
        AvStartAligner al;
        al.on_peek(true, 5'000'000, false, 0, 0);  // 首包可见，记录 first_seen
        auto d = al.on_peek(true, 5'033'000, false, 0, 3'000'001);
        CHECK(d.just_aligned && al.aligned(), "timeout -> forced align");
        CHECK(al.base_us() == 5'033'000, "forced base = current visible pts");
        d = al.on_peek(true, 5'066'000, false, 0, 3'033'001);
        CHECK(d.action == AvStartAligner::Action::Passthrough &&
              al.adjust(5'066'000) == 33'000,
              "post force-align packets adjusted normally");
    }

    // 6. 超时前不强制对齐（保持等待）
    {
        printf("[6] no force-align before timeout\n");
        AvStartAligner al;
        al.on_peek(true, 5'000'000, false, 0, 0);
        auto d = al.on_peek(true, 5'033'000, false, 0, 2'999'999);
        CHECK(d.action == AvStartAligner::Action::Wait && !al.aligned(),
              "still waiting before timeout");
    }

    // 7. reset 恢复初始状态
    {
        printf("[7] reset\n");
        AvStartAligner al;
        al.on_peek(true, 1'000'000, true, 1'400'000, 0);
        CHECK(al.aligned(), "aligned before reset");
        al.reset();
        CHECK(!al.aligned() && al.base_us() == 0, "reset clears state");
        auto d = al.on_peek(true, 2'000'000, false, 0, 0);
        CHECK(d.action == AvStartAligner::Action::Wait, "wait again after reset");
    }

    printf("\n== %s: %d failure(s) ==\n",
           g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
