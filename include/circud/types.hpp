#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TJSMessageClone;
extern "C" void tjs__message_clone_free(TJSMessageClone* clone);

namespace circud {

    inline constexpr std::size_t kMaxEnvelopeBytes = 64 * 1024;
    inline constexpr std::size_t kMaxQueueEntries = 64;
    inline constexpr std::size_t kMaxQueueBytes = 512 * 1024;
    inline constexpr std::size_t kMaxPendingCalls = 64;
    inline constexpr std::size_t kMaxBulkReferences = 16;
    inline constexpr auto kDefaultCallTimeout = std::chrono::seconds(30);
    inline constexpr auto kMaxCallTimeout = std::chrono::minutes(5);

    enum class RuntimeState {
        disabled,
        starting,
        running,
        stopping,
        failed,
        stuck,
    };

    std::string_view to_string(RuntimeState value) noexcept;

    enum class CallErrorCode {
        not_found,
        unavailable,
        package_calls_disabled,
        unhandled_call,
        overloaded,
        timeout,
        cancelled,
        remote_error,
    };

    std::string_view to_string(CallErrorCode value) noexcept;

    struct CallError {
        CallErrorCode code = CallErrorCode::remote_error;
        std::string message;
        std::string remote_name;
        std::string runtime_state;
    };

    struct CloneDeleter {
        void operator()(TJSMessageClone* clone) const noexcept {
            tjs__message_clone_free(clone);
        }
    };
    using ClonePtr = std::unique_ptr<TJSMessageClone, CloneDeleter>;

    struct CallResult {
        ClonePtr value;
        std::optional<std::string> json;
        std::optional<CallError> error;

        static CallResult success(ClonePtr value) {
            CallResult result;
            result.value = std::move(value);
            return result;
        }
        static CallResult success_json(std::string value) {
            CallResult result;
            result.json = std::move(value);
            return result;
        }
        static CallResult failure(CallErrorCode code, std::string message,
            std::string runtime_state = {}) {
            CallResult result;
            result.error = CallError{ code, std::move(message),{},
                                     std::move(runtime_state) };
            return result;
        }
    };

    struct StatusValue {
        std::string state = "unknown";
        std::string message;
    };

    struct StatusSnapshot {
        std::optional<StatusValue> value;
        bool stale = true;
        std::string error;
        std::chrono::system_clock::time_point collected_at{};
        std::chrono::system_clock::time_point last_success_at{};
        unsigned consecutive_timeouts = 0;
    };

} // namespace circud
