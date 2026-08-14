#include "native_video_renderer.h"

#include <android/hardware_buffer.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>

#include "hardware_buffer_frame.h"
#include "streambridge/logging.h"
#include "streambridge/video_utils.h"

namespace streambridge::android {
namespace {

using streambridge::pack_rgba;
using streambridge::yuv_to_rgb;

constexpr char kRenderTag[] = "StreamBridgeRender";

streambridge::Result<void> invalid_frame(const char* message) {
    return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Config,
            streambridge::ErrorCode::InvalidArgument,
            message);
}

streambridge::Result<void> egl_error(const char* message) {
    return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Device,
            streambridge::ErrorCode::DeviceCapUnsupported,
            message);
}

streambridge::Result<void> egl_error_code(const char* message, EGLint code) {
    std::ostringstream oss;
    oss << message << " egl=0x" << std::hex << code;
    return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Device,
            streambridge::ErrorCode::DeviceCapUnsupported,
            oss.str());
}

streambridge::Result<void> gl_error_code(const char* message, GLenum code) {
    std::ostringstream oss;
    oss << message << " gl=0x" << std::hex << code;
    return streambridge::Result<void>::err(
            streambridge::ErrorDomain::Device,
            streambridge::ErrorCode::DeviceCapUnsupported,
            oss.str());
}

}  // namespace

NativeVideoRenderer::NativeVideoRenderer() = default;

NativeVideoRenderer::~NativeVideoRenderer() {
    clear_surface();
}

void NativeVideoRenderer::set_surface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (window_ == window) {
        return;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
    }
    destroy_egl_locked();
    window_ = window;
    last_buf_width_ = -1;
    last_buf_height_ = -1;
    if (window_ != nullptr) {
        ANativeWindow_acquire(window_);
    }
}

void NativeVideoRenderer::clear_surface() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    destroy_egl_locked();
    last_buf_width_ = -1;
    last_buf_height_ = -1;
}

streambridge::Result<void> NativeVideoRenderer::render(const streambridge::VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (window_ == nullptr) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceNotFound,
                "surface is not ready");
    }
    if (!frame.is_valid()) {
        return invalid_frame("video frame is invalid");
    }

    if (frame.memory_type() == streambridge::MemoryType::HardwareBuffer) {
        auto gpu_result = render_hardware_buffer(frame);
        if (gpu_result.is_ok()) {
            return gpu_result;
        }
        SB_LOG_W(kRenderTag, "AHardwareBuffer GPU render failed, fallback CPU path: %s",
                 gpu_result.error_message().c_str());
    }

    switch (frame.format) {
        case streambridge::PixelFormat::RGBA:
            return render_rgba(frame, false);
        case streambridge::PixelFormat::BGRA:
            return render_rgba(frame, true);
        case streambridge::PixelFormat::YUV420P:
            return render_yuv420p(frame);
        case streambridge::PixelFormat::NV12:
            return render_nv12_nv21(frame, false);
        case streambridge::PixelFormat::NV21:
            return render_nv12_nv21(frame, true);
        default:
            return streambridge::Result<void>::err(
                    streambridge::ErrorDomain::Codec,
                    streambridge::ErrorCode::CodecFormatUnsupported,
                    "unsupported video frame format");
    }
}

streambridge::Result<void> NativeVideoRenderer::render_rgba(
        const streambridge::VideoFrame& frame,
        bool source_is_bgra) {
    if (frame.num_planes < 1 || frame.planes[0].data == nullptr || frame.planes[0].stride <= 0) {
        return invalid_frame("RGBA frame plane is invalid");
    }

    // Keep the native buffer in source-frame geometry. This avoids an extra
    // software scaling pass and, more importantly, avoids device-dependent
    // Surface buffer stride/crop behavior when geometry is reset to 0x0.
    if (last_buf_width_ != frame.width || last_buf_height_ != frame.height) {
        ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = frame.width;
        last_buf_height_ = frame.height;
        SB_LOG_I(kRenderTag, "buffer geometry set to frame size %dx%d",
                 frame.width, frame.height);
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    const uint8_t* src = frame.planes[0].data;
    const int src_w = frame.width;
    const int src_h = frame.height;
    const int src_stride = frame.planes[0].stride;

    const int buf_w = static_cast<int>(buffer.width);
    const int buf_h = static_cast<int>(buffer.height);

    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int copy_w = std::min(src_w, buf_w);
    const int copy_h = std::min(src_h, buf_h);

    for (int y = 0; y < copy_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* src_row = src + y * src_stride;
        if (!source_is_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * 4);
            continue;
        }
        for (int x = 0; x < copy_w; ++x) {
            const uint8_t* px = src_row + x * 4;
            dst_row[x] = pack_rgba(px[2], px[1], px[0], px[3]);
        }
    }

    // Diagnostic: first frame only
    static int frame_count = 0;
    if (frame_count++ == 0) {
        SB_LOG_I(kRenderTag,
                "first render: src=%dx%d(stride=%d) buf=%dx%d(stride=%d) lb=%d,%d %dx%d scale=%.3f,%.3f",
                src_w, src_h, src_stride, buf_w, buf_h, (int)buffer.stride,
                0, 0, copy_w, copy_h, 1.0, 1.0);
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

streambridge::Result<void> NativeVideoRenderer::render_yuv420p(
        const streambridge::VideoFrame& frame) {
    if (frame.num_planes < 3 ||
            frame.planes[0].data == nullptr ||
            frame.planes[1].data == nullptr ||
            frame.planes[2].data == nullptr) {
        return invalid_frame("YUV420P frame planes are invalid");
    }

    if (last_buf_width_ != frame.width || last_buf_height_ != frame.height) {
        ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = frame.width;
        last_buf_height_ = frame.height;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    const int src_w = frame.width;
    const int src_h = frame.height;
    const int buf_w = static_cast<int>(buffer.width);
    const int buf_h = static_cast<int>(buffer.height);
    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int copy_w = std::min(src_w, buf_w);
    const int copy_h = std::min(src_h, buf_h);

    for (int y = 0; y < copy_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* y_row = frame.planes[0].data + y * frame.planes[0].stride;
        const uint8_t* u_row = frame.planes[1].data + (y / 2) * frame.planes[1].stride;
        const uint8_t* v_row = frame.planes[2].data + (y / 2) * frame.planes[2].stride;

        for (int x = 0; x < copy_w; ++x) {
            uint8_t r, g, b;
            yuv_to_rgb(static_cast<int>(y_row[x]),
                       static_cast<int>(u_row[x / 2]),
                       static_cast<int>(v_row[x / 2]), r, g, b);
            dst_row[x] = pack_rgba(r, g, b);
        }
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

streambridge::Result<void> NativeVideoRenderer::render_nv12_nv21(
        const streambridge::VideoFrame& frame,
        bool is_nv21) {
    if (frame.num_planes < 2 ||
            frame.planes[0].data == nullptr ||
            frame.planes[1].data == nullptr) {
        return invalid_frame("NV12/NV21 frame planes are invalid");
    }

    if (last_buf_width_ != frame.width || last_buf_height_ != frame.height) {
        ANativeWindow_setBuffersGeometry(window_, frame.width, frame.height,
                                         WINDOW_FORMAT_RGBA_8888);
        last_buf_width_ = frame.width;
        last_buf_height_ = frame.height;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
        return streambridge::Result<void>::err(
                streambridge::ErrorDomain::Device,
                streambridge::ErrorCode::DeviceBusy,
                "failed to lock surface");
    }

    const int src_w = frame.width;
    const int src_h = frame.height;
    const int buf_w = static_cast<int>(buffer.width);
    const int buf_h = static_cast<int>(buffer.height);
    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int copy_w = std::min(src_w, buf_w);
    const int copy_h = std::min(src_h, buf_h);

    for (int y = 0; y < copy_h; ++y) {
        uint32_t* dst_row = dst + y * buffer.stride;
        const uint8_t* y_row = frame.planes[0].data + y * frame.planes[0].stride;
        const uint8_t* uv_row = frame.planes[1].data + (y / 2) * frame.planes[1].stride;

        for (int x = 0; x < copy_w; ++x) {
            const uint8_t* uv = uv_row + (x / 2) * 2;
            const uint8_t u = is_nv21 ? uv[1] : uv[0];
            const uint8_t v = is_nv21 ? uv[0] : uv[1];
            uint8_t r, g, b;
            yuv_to_rgb(static_cast<int>(y_row[x]), static_cast<int>(u),
                       static_cast<int>(v), r, g, b);
            dst_row[x] = pack_rgba(r, g, b);
        }
    }

    ANativeWindow_unlockAndPost(window_);
    return streambridge::Result<void>::ok();
}

streambridge::Result<void> NativeVideoRenderer::render_hardware_buffer(
        const streambridge::VideoFrame& frame) {
    auto hardware_buffer =
        std::static_pointer_cast<HardwareBufferFrameBuffer>(frame.buffer);
    if (!hardware_buffer || hardware_buffer->buffer() == nullptr) {
        return invalid_frame("hardware buffer frame has no AHardwareBuffer");
    }

    auto egl_result = ensure_egl_locked();
    if (egl_result.is_err()) {
        return egl_result;
    }

    const EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLClientBuffer client_buffer =
        eglGetNativeClientBufferANDROID(hardware_buffer->buffer());
    if (client_buffer == nullptr) {
        return egl_error("eglGetNativeClientBufferANDROID failed");
    }

    EGLImageKHR image = eglCreateImageKHR(
        egl_display_,
        EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID,
        client_buffer,
        image_attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        return egl_error("eglCreateImageKHR failed");
    }

    glViewport(0, 0, frame.width, frame.height);
    glUseProgram(gl_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl_texture_);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
    GLenum gl_code = glGetError();
    if (gl_code != GL_NO_ERROR) {
        eglDestroyImageKHR(egl_display_, image);
        return gl_error_code("glEGLImageTargetTexture2DOES failed", gl_code);
    }
    glUniform1i(gl_sampler_loc_, 0);

    static constexpr GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    glVertexAttribPointer(gl_pos_loc_, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), vertices);
    glEnableVertexAttribArray(gl_pos_loc_);
    glVertexAttribPointer(gl_tex_loc_, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray(gl_tex_loc_);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_code = glGetError();
    if (gl_code != GL_NO_ERROR) {
        eglDestroyImageKHR(egl_display_, image);
        return gl_error_code("glDrawArrays failed", gl_code);
    }
    glDisableVertexAttribArray(gl_pos_loc_);
    glDisableVertexAttribArray(gl_tex_loc_);

    if (eglSwapBuffers(egl_display_, egl_surface_) != EGL_TRUE) {
        const EGLint egl_code = eglGetError();
        eglDestroyImageKHR(egl_display_, image);
        return egl_error_code("eglSwapBuffers failed", egl_code);
    }
    eglDestroyImageKHR(egl_display_, image);

    static int logged = 0;
    if (logged++ < 3) {
        SB_LOG_I(kRenderTag, "rendered AHardwareBuffer via EGLImage %dx%d stride=%d fmt=%u",
                 hardware_buffer->width(), hardware_buffer->height(),
                 hardware_buffer->stride_pixels(), hardware_buffer->format());
    }
    return streambridge::Result<void>::ok();
}

streambridge::Result<void> NativeVideoRenderer::ensure_egl_locked() {
    if (egl_display_ != EGL_NO_DISPLAY &&
            egl_context_ != EGL_NO_CONTEXT &&
            egl_surface_ != EGL_NO_SURFACE &&
            gl_program_ != 0) {
        eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
        const EGLint egl_code = eglGetError();
        if (egl_code != EGL_SUCCESS) {
            destroy_egl_locked();
            return egl_error_code("eglMakeCurrent existing context failed", egl_code);
        }
        return streambridge::Result<void>::ok();
    }

    if (window_ == nullptr) {
        return egl_error("surface is not ready for EGL");
    }

    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY) {
        return egl_error_code("eglGetDisplay failed", eglGetError());
    }
    if (eglInitialize(egl_display_, nullptr, nullptr) != EGL_TRUE) {
        const EGLint egl_code = eglGetError();
        destroy_egl_locked();
        return egl_error_code("eglInitialize failed", egl_code);
    }

    const EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint num_configs = 0;
    if (eglChooseConfig(egl_display_, config_attrs, &egl_config_, 1, &num_configs) != EGL_TRUE ||
            num_configs <= 0) {
        const EGLint egl_code = eglGetError();
        destroy_egl_locked();
        return egl_error_code("eglChooseConfig failed", egl_code);
    }

    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attrs);
    if (egl_context_ == EGL_NO_CONTEXT) {
        const EGLint egl_code = eglGetError();
        destroy_egl_locked();
        return egl_error_code("eglCreateContext failed", egl_code);
    }

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, window_, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        const EGLint egl_code = eglGetError();
        destroy_egl_locked();
        return egl_error_code("eglCreateWindowSurface failed", egl_code);
    }
    if (eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) != EGL_TRUE) {
        const EGLint egl_code = eglGetError();
        destroy_egl_locked();
        return egl_error_code("eglMakeCurrent failed", egl_code);
    }

    auto program_result = create_gl_program_locked();
    if (program_result.is_err()) {
        destroy_egl_locked();
        return program_result;
    }

    glGenTextures(1, &gl_texture_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl_texture_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum tex_error = glGetError();
    if (tex_error != GL_NO_ERROR) {
        destroy_egl_locked();
        return gl_error_code("external texture setup failed", tex_error);
    }

    SB_LOG_I(kRenderTag, "EGL renderer initialized");
    return streambridge::Result<void>::ok();
}

void NativeVideoRenderer::destroy_egl_locked() {
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (gl_texture_ != 0) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
    if (gl_program_ != 0) {
        glDeleteProgram(gl_program_);
        gl_program_ = 0;
    }
    if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
    }
    if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display_, egl_context_);
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
    }
    egl_display_ = EGL_NO_DISPLAY;
    egl_context_ = EGL_NO_CONTEXT;
    egl_surface_ = EGL_NO_SURFACE;
    egl_config_ = nullptr;
    gl_pos_loc_ = -1;
    gl_tex_loc_ = -1;
    gl_sampler_loc_ = -1;
}

GLuint NativeVideoRenderer::compile_shader_locked(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        SB_LOG_E(kRenderTag, "shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

streambridge::Result<void> NativeVideoRenderer::create_gl_program_locked() {
    static constexpr const char* kVertexShader = R"(
        attribute vec2 aPosition;
        attribute vec2 aTexCoord;
        varying vec2 vTexCoord;
        void main() {
            gl_Position = vec4(aPosition, 0.0, 1.0);
            vTexCoord = aTexCoord;
        }
    )";
    static constexpr const char* kFragmentShader = R"(
        #extension GL_OES_EGL_image_external : require
        precision mediump float;
        varying vec2 vTexCoord;
        uniform samplerExternalOES uTexture;
        void main() {
            gl_FragColor = texture2D(uTexture, vTexCoord);
        }
    )";

    GLuint vs = compile_shader_locked(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile_shader_locked(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return egl_error("shader compile failed");
    }

    gl_program_ = glCreateProgram();
    glAttachShader(gl_program_, vs);
    glAttachShader(gl_program_, fs);
    glLinkProgram(gl_program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(gl_program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[256] = {};
        glGetProgramInfoLog(gl_program_, sizeof(log), nullptr, log);
        SB_LOG_E(kRenderTag, "program link failed: %s", log);
        glDeleteProgram(gl_program_);
        gl_program_ = 0;
        return egl_error("program link failed");
    }

    gl_pos_loc_ = glGetAttribLocation(gl_program_, "aPosition");
    gl_tex_loc_ = glGetAttribLocation(gl_program_, "aTexCoord");
    gl_sampler_loc_ = glGetUniformLocation(gl_program_, "uTexture");
    if (gl_pos_loc_ < 0 || gl_tex_loc_ < 0 || gl_sampler_loc_ < 0) {
        glDeleteProgram(gl_program_);
        gl_program_ = 0;
        return egl_error("program locations unavailable");
    }

    return streambridge::Result<void>::ok();
}

}  // namespace streambridge::android
