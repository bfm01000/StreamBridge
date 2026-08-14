#pragma once
// Common logging API — platform-independent
// Each platform provides log_write() implementation linked at build time

#include <cstdarg>

namespace streambridge {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

// Platform must implement: format and write a log message
// Uses printf-style format with __attribute__((format(printf))) for compiler checks
void log_write(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Global log level filter (implemented in logging.cpp)
void set_log_level(LogLevel level);
LogLevel get_log_level();

}  // namespace streambridge

// ============================================================
// Convenience macros — use these in all code
// ============================================================

// 便捷日志宏：统一日志输出入口，按 tag 区分模块，全部下发到 log_write
#define SB_LOG_D(tag, fmt, ...) \
    streambridge::log_write(streambridge::LogLevel::Debug, tag, fmt, ##__VA_ARGS__)

#define SB_LOG_I(tag, fmt, ...) \
    streambridge::log_write(streambridge::LogLevel::Info,  tag, fmt, ##__VA_ARGS__)

#define SB_LOG_W(tag, fmt, ...) \
    streambridge::log_write(streambridge::LogLevel::Warn,  tag, fmt, ##__VA_ARGS__)

#define SB_LOG_E(tag, fmt, ...) \
    streambridge::log_write(streambridge::LogLevel::Error, tag, fmt, ##__VA_ARGS__)
