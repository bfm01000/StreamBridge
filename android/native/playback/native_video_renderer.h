#pragma once

#include <android/native_window.h>
#include <mutex>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

// ANativeWindow 视频渲染器：负责在 Surface 上渲染 RGBA 与 YUV420P 视频帧，并管理窗口生命周期。
class NativeVideoRenderer {
public:
    NativeVideoRenderer();
    ~NativeVideoRenderer();

    NativeVideoRenderer(const NativeVideoRenderer&) = delete;
    NativeVideoRenderer& operator=(const NativeVideoRenderer&) = delete;

    void set_surface(ANativeWindow* window);
    void clear_surface();
    streambridge::Result<void> render(const streambridge::VideoFrame& frame);
    ANativeWindow* window() const { return window_; }

private:
    streambridge::Result<void> render_rgba(const streambridge::VideoFrame& frame,
                                           bool source_is_bgra);
    streambridge::Result<void> render_yuv420p(const streambridge::VideoFrame& frame);

    std::mutex render_mutex_;           // protects window_ and render ops
    ANativeWindow* window_ = nullptr;
    int last_buf_width_ = -1;
    int last_buf_height_ = -1;
};

}  // namespace streambridge::android
