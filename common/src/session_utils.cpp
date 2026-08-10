#include "streambridge/session.h"

namespace streambridge {

const char* session_state_name(SessionState state) {
    switch (state) {
        case SessionState::Idle:         return "Idle";
        case SessionState::Preparing:    return "Preparing";
        case SessionState::Prepared:     return "Prepared";
        case SessionState::Running:      return "Running";
        case SessionState::Paused:       return "Paused";
        case SessionState::Reconnecting: return "Reconnecting";
        case SessionState::Stopping:     return "Stopping";
        case SessionState::Stopped:      return "Stopped";
        case SessionState::Error:        return "Error";
    }
    return "Unknown";
}

}  // namespace streambridge
