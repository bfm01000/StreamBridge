#pragma once
// 简单的控制台日志

#include <cstdio>
#include <string>

namespace streambridge {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

void set_log_level(LogLevel level);
LogLevel log_level();

// 简易日志宏
#define LOG_D(module, fmt, ...) \
    log_msg(LogLevel::Debug, module, fmt, ##__VA_ARGS__)
#define LOG_I(module, fmt, ...) \
    log_msg(LogLevel::Info, module, fmt, ##__VA_ARGS__)
#define LOG_W(module, fmt, ...) \
    log_msg(LogLevel::Warn, module, fmt, ##__VA_ARGS__)
#define LOG_E(module, fmt, ...) \
    log_msg(LogLevel::Error, module, fmt, ##__VA_ARGS__)

void log_msg(LogLevel level, const char* module, const char* fmt, ...);

}  // namespace streambridge
