#pragma once
// Backward-compatibility wrapper — includes the common logging header
// All code should eventually include <streambridge/logging.h> directly.
// This file exists so existing code that includes "../logger.h" continues to compile.

#include "streambridge/logging.h"

// Old API compat aliases
namespace streambridge {
// set_log_level / get_log_level are now declared in streambridge/logging.h
// so including this header gives you both SB_LOG_* macros and level control.
}  // namespace streambridge
