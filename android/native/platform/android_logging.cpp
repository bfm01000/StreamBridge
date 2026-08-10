// Android platform logging — maps to logcat via __android_log_vprint
#include "streambridge/logging.h"

#include <android/log.h>

namespace streambridge {

void log_write(LogLevel level, const char* tag, const char* fmt, ...) {
    android_LogPriority prio;
    switch (level) {
        case LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info:  prio = ANDROID_LOG_INFO;  break;
        case LogLevel::Warn:  prio = ANDROID_LOG_WARN;  break;
        case LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
        default:              prio = ANDROID_LOG_DEFAULT;
    }

    va_list args;
    va_start(args, fmt);
    __android_log_vprint(prio, tag, fmt, args);
    va_end(args);
}

}  // namespace streambridge
