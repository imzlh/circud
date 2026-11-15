#include <circud/types.hpp>

namespace circud {

    std::string_view to_string(RuntimeState value) noexcept {
        switch (value) {
        case RuntimeState::disabled: return "disabled";
        case RuntimeState::starting: return "starting";
        case RuntimeState::running: return "running";
        case RuntimeState::stopping: return "stopping";
        case RuntimeState::failed: return "failed";
        case RuntimeState::stuck: return "stuck";
        }
        return "unknown";
    }

    std::string_view to_string(CallErrorCode value) noexcept {
        switch (value) {
        case CallErrorCode::not_found: return "not_found";
        case CallErrorCode::unavailable: return "unavailable";
        case CallErrorCode::package_calls_disabled:
            return "package_calls_disabled";
        case CallErrorCode::unhandled_call: return "unhandled_call";
        case CallErrorCode::overloaded: return "overloaded";
        case CallErrorCode::timeout: return "timeout";
        case CallErrorCode::cancelled: return "cancelled";
        case CallErrorCode::remote_error: return "remote_error";
        }
        return "remote_error";
    }

} // namespace circud
