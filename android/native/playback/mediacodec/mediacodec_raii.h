#pragma once
// MediaCodec NDK RAII wrappers

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <memory>

namespace streambridge::android::mediacodec {

// AMediaCodec 的 RAII 删除器：停止并释放 MediaCodec 实例。
struct AMediaCodecDeleter {
    void operator()(AMediaCodec* c) const {
        if (c) {
            AMediaCodec_stop(c);
            AMediaCodec_delete(c);
        }
    }
};
// 持有 AMediaCodec 的唯一指针别名，生命周期由 AMediaCodecDeleter 自动管理。
using AMediaCodecPtr = std::unique_ptr<AMediaCodec, AMediaCodecDeleter>;

// AMediaFormat 的 RAII 删除器：释放 MediaFormat 实例。
struct AMediaFormatDeleter {
    void operator()(AMediaFormat* f) const {
        if (f) AMediaFormat_delete(f);
    }
};
// 持有 AMediaFormat 的唯一指针别名，生命周期由 AMediaFormatDeleter 自动管理。
using AMediaFormatPtr = std::unique_ptr<AMediaFormat, AMediaFormatDeleter>;

}  // namespace streambridge::android::mediacodec
