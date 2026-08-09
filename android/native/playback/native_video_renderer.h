#pragma once

#include <android/native_window.h>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge::android {

class NativeVideoRenderer {
public:
    NativeVideoRenderer();
    ~NativeVideoRenderer();

    NativeVideoRenderer(const NativeVideoRenderer&) = delete;
    NativeVideoRenderer& operator=(const NativeVideoRenderer&) = delete;

    void set_surface(ANativeWindow* window);
    void clear_surface();
    streambridge::Result<void> render(const streambridge::VideoFrame& frame);

private:
    streambridge::Result<void> render_rgba(const streambridge::VideoFrame& frame,
                                           bool source_is_bgra);
    streambridge::Result<void> render_yuv420p(const streambridge::VideoFrame& frame);

    ANativeWindow* window_ = nullptr;
};

}  // namespace streambridge::android
