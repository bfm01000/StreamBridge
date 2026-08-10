#pragma once
// MediaCodec NDK RAII wrappers

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <memory>

namespace streambridge::android::mediacodec {

struct AMediaCodecDeleter {
    void operator()(AMediaCodec* c) const {
        if (c) {
            AMediaCodec_stop(c);
            AMediaCodec_delete(c);
        }
    }
};
using AMediaCodecPtr = std::unique_ptr<AMediaCodec, AMediaCodecDeleter>;

struct AMediaFormatDeleter {
    void operator()(AMediaFormat* f) const {
        if (f) AMediaFormat_delete(f);
    }
};
using AMediaFormatPtr = std::unique_ptr<AMediaFormat, AMediaFormatDeleter>;

}  // namespace streambridge::android::mediacodec
