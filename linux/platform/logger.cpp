#include "logger.h"
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <mutex>

namespace streambridge {

static LogLevel g_log_level = LogLevel::Info;
static std::mutex g_log_mutex;

void set_log_level(LogLevel level) { g_log_level = level; }
LogLevel log_level() { return g_log_level; }

void log_msg(LogLevel level, const char* module, const char* fmt, ...) {
    if (level < g_log_level) return;

    std::lock_guard<std::mutex> lock(g_log_mutex);

    // 时间戳
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count() % 1'000'000;

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", localtime(&now_t));

    // 日志级别
    const char* level_str = "?";
    switch (level) {
        case LogLevel::Debug: level_str = "D"; break;
        case LogLevel::Info:  level_str = "I"; break;
        case LogLevel::Warn:  level_str = "W"; break;
        case LogLevel::Error: level_str = "E"; break;
    }

    fprintf(stderr, "[%s.%06ld] [%s] [%s] ",
            time_buf, us, level_str, module);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

}  // namespace streambridge
