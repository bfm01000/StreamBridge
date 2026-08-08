#pragma once
// 命令行解析 → PublishSessionConfig

#include "streambridge/session.h"

namespace streambridge {

enum class AppMode {
    Publisher,
    Player,  // 未来
};

struct AppConfig {
    AppMode mode = AppMode::Publisher;
    std::string rtmp_url;
    std::string video_source;
    int video_width = 1280;
    int video_height = 720;
    int video_fps = 30;
    int video_bitrate = 2'000'000;
    // 音频选项（M3+）
    bool enable_audio = false;
    std::string audio_source;       // lavfi filter / file path / ALSA device
    std::string audio_backend = "alsa";  // "alsa" (default), "lavfi", "file"
    int audio_sample_rate = 48000;
    int audio_channels = 2;
    int audio_bitrate = 128'000;
    // 视频 backend
    std::string video_backend;      // "lavfi" (default), "file", "v4l2"
    bool loop = false;
    bool no_throttle = false;
    std::string log_level_str = "info";

    // 解析命令行
    static Result<AppConfig> parse(int argc, char* argv[]);

    // 转为 SessionConfig
    PublishSessionConfig to_session_config() const;
};

void print_usage(const char* prog);

}  // namespace streambridge
