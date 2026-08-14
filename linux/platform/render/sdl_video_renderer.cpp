#include "sdl_video_renderer.h"

#include <cstring>

#include "streambridge/logging.h"
#include "streambridge/video_utils.h"

namespace streambridge {

SDLVideoRenderer::~SDLVideoRenderer() {
    close();
}

Result<void> SDLVideoRenderer::open(const char* title, int window_width, int window_height) {
    // 虚拟机/无 GPU 环境下 GL 加速渲染可能崩溃（GLX/llvmpipe），
    // 第一版固定软件渲染：720p YUV→RGB 软件转换 + software renderer 足够；
    // 同时强制 x11 驱动——环境设置了 WAYLAND_DISPLAY 时 SDL 会优先
    // Wayland，虚拟机内 Wayland 可能不可用导致窗口创建崩溃
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string("SDL_Init failed: ") + SDL_GetError());
    }
    SB_LOG_D("render", "creating window %dx%d...", window_width, window_height);
    window_ = SDL_CreateWindow(title,
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               window_width, window_height,
                               SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    SB_LOG_D("render", "window created, creating renderer...");
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
    }
    win_w_ = window_width;
    win_h_ = window_height;
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RendererInfo rinfo{};
    const char* renderer_name =
        (SDL_GetRendererInfo(renderer_, &rinfo) == 0) ? rinfo.name : "unknown";
    SB_LOG_I("render", "SDL window %dx%d created (renderer=%s)",
          window_width, window_height, renderer_name);
    return Result<void>::ok();
}

void SDLVideoRenderer::close() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    texture_ = nullptr;
    renderer_ = nullptr;
    window_ = nullptr;
    tex_w_ = tex_h_ = 0;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

Result<void> SDLVideoRenderer::ensure_texture(int frame_width, int frame_height) {
    if (texture_ && tex_w_ == frame_width && tex_h_ == frame_height) {
        return Result<void>::ok();
    }
    if (texture_) SDL_DestroyTexture(texture_);
    // streaming 纹理：每帧 SDL_UpdateTexture 整帧覆盖
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 frame_width, frame_height);
    if (!texture_) {
        return Result<void>::err(ErrorDomain::Device, ErrorCode::OutOfMemory,
                                 std::string("SDL_CreateTexture failed: ") + SDL_GetError());
    }
    tex_w_ = frame_width;
    tex_h_ = frame_height;
    rgba_buf_.resize(static_cast<size_t>(frame_width) * frame_height);
    SB_LOG_I("render", "video texture %dx%d created", frame_width, frame_height);
    return Result<void>::ok();
}

Result<void> SDLVideoRenderer::render(const VideoFrame& frame) {
    if (!renderer_ || !frame.is_valid()) {
        return Result<void>::err(ErrorDomain::Config, ErrorCode::InvalidConfig,
                                 "renderer not ready or invalid frame");
    }

    auto ret = ensure_texture(frame.width, frame.height);
    if (ret.is_err()) return ret;

    uint32_t* dst = rgba_buf_.data();

    if (frame.format == PixelFormat::YUV420P && frame.num_planes >= 3) {
        const uint8_t* y = frame.planes[0].data;
        const uint8_t* u = frame.planes[1].data;
        const uint8_t* v = frame.planes[2].data;
        const int y_stride = frame.planes[0].stride > 0 ? frame.planes[0].stride : frame.width;
        const int uv_stride = frame.planes[1].stride > 0 ? frame.planes[1].stride : (frame.width + 1) / 2;

        for (int row = 0; row < frame.height; row++) {
            for (int col = 0; col < frame.width; col++) {
                int yy = y[row * y_stride + col];
                int uu = u[(row / 2) * uv_stride + col / 2];
                int vv = v[(row / 2) * uv_stride + col / 2];
                uint8_t r, g, b;
                yuv_to_rgb(yy, uu, vv, r, g, b);
                dst[static_cast<size_t>(row) * frame.width + col] = pack_rgba(r, g, b);
            }
        }
    } else if (frame.format == PixelFormat::RGBA && frame.num_planes >= 1) {
        memcpy(dst, frame.planes[0].data, rgba_buf_.size() * sizeof(uint32_t));
    } else {
        return Result<void>::err(ErrorDomain::Device, ErrorCode::CodecFormatUnsupported,
                                 "unsupported pixel format for SDL renderer");
    }

    if (SDL_UpdateTexture(texture_, nullptr, rgba_buf_.data(),
                          frame.width * static_cast<int>(sizeof(uint32_t))) != 0) {
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string("SDL_UpdateTexture failed: ") + SDL_GetError());
    }

    // 等比居中（letterbox）
    SDL_GetWindowSize(window_, &win_w_, &win_h_);
    LetterBox lb = calc_letterbox(frame.width, frame.height, win_w_, win_h_);
    SDL_Rect dst_rect{lb.x, lb.y, lb.w, lb.h};

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst_rect);
    SDL_RenderPresent(renderer_);
    return Result<void>::ok();
}

bool SDLVideoRenderer::poll_events(bool& quit_requested) {
    quit_requested = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            quit_requested = true;
            return true;
        }
        if (event.type == SDL_KEYDOWN &&
            (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q)) {
            quit_requested = true;
            return true;
        }
    }
    return true;
}

}  // namespace streambridge
