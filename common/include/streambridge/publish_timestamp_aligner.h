#pragma once

#include <algorithm>
#include <cstdint>

#include "streambridge/media_types.h"

namespace streambridge {

class PublishTimestampAligner {
public:
    enum class Action {
        WaitForPeer,
        DropBeforeBase,
        Pass,
    };

    struct Decision {
        Action action = Action::WaitForPeer;
        TimePointUs normalized_pts{0};
        TimePointUs base_pts{0};
        int64_t av_diff_us = 0;
        bool just_aligned = false;
    };

    void reset() {
        have_video_ = false;
        have_audio_ = false;
        aligned_ = false;
        first_video_pts_ = TimePointUs{0};
        first_audio_pts_ = TimePointUs{0};
        latest_video_pts_ = TimePointUs{0};
        latest_audio_pts_ = TimePointUs{0};
        base_pts_ = TimePointUs{0};
    }

    Decision on_packet(MediaType type, TimePointUs pts) {
        bool just_aligned = false;
        if (type == MediaType::Video) {
            if (!have_video_) {
                first_video_pts_ = pts;
                have_video_ = true;
            }
            latest_video_pts_ = pts;
        } else if (type == MediaType::Audio) {
            if (!have_audio_) {
                first_audio_pts_ = pts;
                have_audio_ = true;
            }
            latest_audio_pts_ = pts;
        }

        if (!aligned_) {
            if (!have_video_ || !have_audio_) {
                return {Action::WaitForPeer, TimePointUs{0}, base_pts_,
                        av_diff_us(), false};
            }
            base_pts_ = std::max(first_video_pts_, first_audio_pts_);
            aligned_ = true;
            just_aligned = true;
        }

        if (pts < base_pts_) {
            return {Action::DropBeforeBase, TimePointUs{0}, base_pts_,
                    av_diff_us(), just_aligned};
        }
        return {Action::Pass, TimePointUs{pts.us - base_pts_.us}, base_pts_,
                av_diff_us(), just_aligned};
    }

    bool aligned() const { return aligned_; }
    TimePointUs base_pts() const { return base_pts_; }
    int64_t av_diff_us() const {
        if (!have_video_ || !have_audio_) {
            return 0;
        }
        return (latest_video_pts_ - latest_audio_pts_).us;
    }

private:
    bool have_video_ = false;
    bool have_audio_ = false;
    bool aligned_ = false;
    TimePointUs first_video_pts_{0};
    TimePointUs first_audio_pts_{0};
    TimePointUs latest_video_pts_{0};
    TimePointUs latest_audio_pts_{0};
    TimePointUs base_pts_{0};
};

}  // namespace streambridge
