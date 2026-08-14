// POSIX platform logging — maps to stderr with timestamp
#include "streambridge/logging.h"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace streambridge {

static LogLevel g_log_level = LogLevel::Info;
static std::mutex g_log_mutex;

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_level = level;
}

LogLevel get_log_level() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_log_level;
}

void log_write(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level < g_log_level) return;

    const char* level_str = "?";
    switch (level) {
        case LogLevel::Debug: level_str = "D"; break;
        case LogLevel::Info:  level_str = "I"; break;
        case LogLevel::Warn:  level_str = "W"; break;
        case LogLevel::Error: level_str = "E"; break;
    }

    std::lock_guard<std::mutex> lock(g_log_mutex);

    // Timestamp prefix
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%lld.%06ld] %s/%s: ",
            static_cast<long long>(ts.tv_sec),
            ts.tv_nsec / 1000,
            level_str, tag);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

}  // namespace streambridge
