#pragma once
// SDL2 软件渲染器 — 第一版 Linux 播放端视频输出
// YUV420P（CPU 帧）→ RGBA8888 → SDL 纹理 → 窗口（letterbox 等比居中）
// 架构依据：docs/architecture.md — 反向链路第一版建议 SDL2，
// 便于窗口管理与软件渲染验证。

#include <cstdint>
#include <string>
#include <vector>

#include <SDL.h>

#include "streambridge/media_errors.h"
#include "streambridge/media_types.h"

namespace streambridge {

class SDLVideoRenderer {
public:
    SDLVideoRenderer() = default;
    ~SDLVideoRenderer();

    SDLVideoRenderer(const SDLVideoRenderer&) = delete;
    SDLVideoRenderer& operator=(const SDLVideoRenderer&) = delete;

    // 创建窗口（窗口尺寸）与渲染器；视频帧纹理按首帧尺寸惰性创建
    Result<void> open(const char* title, int window_width, int window_height);
    void close();
    bool is_open() const { return window_ != nullptr; }

    // 渲染一帧（阻塞直到上屏；仅支持 CPU YUV420P/RGBA 帧）
    Result<void> render(const VideoFrame& frame);

    // 处理窗口事件；quit_requested 置位表示用户按 ESC 或关闭窗口
    // 返回 false 表示 SDL 内部出错（如窗口被销毁）
    bool poll_events(bool& quit_requested);

    int window_width() const { return win_w_; }
    int window_height() const { return win_h_; }

private:
    Result<void> ensure_texture(int frame_width, int frame_height);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int tex_w_ = 0;
    int tex_h_ = 0;
    int win_w_ = 0;
    int win_h_ = 0;
    std::vector<uint32_t> rgba_buf_;  // YUV→RGB 转换缓冲
};

}  // namespace streambridge
