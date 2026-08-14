#pragma once

#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES 1
#endif
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif

#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
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
    streambridge::Result<void> render_nv12_nv21(const streambridge::VideoFrame& frame,
                                                bool is_nv21);
    streambridge::Result<void> render_hardware_buffer(const streambridge::VideoFrame& frame);

    streambridge::Result<void> ensure_egl_locked();
    void destroy_egl_locked();
    GLuint compile_shader_locked(GLenum type, const char* source);
    streambridge::Result<void> create_gl_program_locked();

    std::mutex render_mutex_;           // protects window_ and render ops
    ANativeWindow* window_ = nullptr;
    int last_buf_width_ = -1;
    int last_buf_height_ = -1;

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLConfig egl_config_ = nullptr;
    GLuint gl_program_ = 0;
    GLuint gl_texture_ = 0;
    GLint gl_pos_loc_ = -1;
    GLint gl_tex_loc_ = -1;
    GLint gl_sampler_loc_ = -1;
};

}  // namespace streambridge::android
