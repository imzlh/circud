#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace circud {

    struct ScryptParameters {
        unsigned log_n = 15;
        unsigned block_size = 8;
        unsigned parallelism = 1;
        std::size_t salt_bytes = 16;
        std::uint64_t max_memory_bytes = 64ULL * 1024 * 1024;
    };

    struct AuthOptions {
        ScryptParameters scrypt;
        std::size_t max_sessions = 128;
        std::chrono::milliseconds idle_timeout{ 30 * 60 * 1000 };
        std::chrono::milliseconds absolute_timeout{ 24 * 60 * 60 * 1000 };
        std::chrono::milliseconds login_window{ 60 * 1000 };
        std::size_t max_login_attempts_per_peer = 5;
        std::size_t max_login_attempts_global = 50;
        std::size_t max_tracked_peers = 1024;
        std::size_t max_pending_logins = 16;
        // The current implementation deliberately owns one crypto worker.
        // Keep this explicit so configuration cannot imply hidden parallelism.
        std::size_t max_scrypt_inflight = 1;
    };

    enum class LoginStatus {
        authenticated,
        invalid_credentials,
        rate_limited,
        invalid_request,
        invalid_hash,
        unavailable,
    };

    struct LoginResult {
        LoginStatus status = LoginStatus::unavailable;
        std::string token;
        std::string csrf_token;
        std::chrono::milliseconds retry_after{};
        std::chrono::milliseconds idle_lifetime{};
        std::chrono::milliseconds absolute_lifetime{};
        std::chrono::steady_clock::time_point idle_expires_at{};
        std::chrono::steady_clock::time_point absolute_expires_at{};

        explicit operator bool() const noexcept {
            return status == LoginStatus::authenticated;
        }
    };

    enum class LoginSubmissionStatus {
        accepted,
        invalid_request,
        invalid_hash,
        rate_limited,
        overloaded,
        stopping,
    };

    struct LoginSubmission {
        LoginSubmissionStatus status = LoginSubmissionStatus::overloaded;
        std::chrono::milliseconds retry_after{};

        explicit operator bool() const noexcept {
            return status == LoginSubmissionStatus::accepted;
        }
    };

    enum class AuthenticationStatus {
        authenticated,
        invalid_token,
        csrf_mismatch,
    };

    struct AuthenticationResult {
        AuthenticationStatus status = AuthenticationStatus::invalid_token;
        std::string csrf_token;
        std::chrono::milliseconds idle_remaining{};
        std::chrono::milliseconds absolute_remaining{};
        std::chrono::steady_clock::time_point idle_expires_at{};
        std::chrono::steady_clock::time_point absolute_expires_at{};

        explicit operator bool() const noexcept {
            return status == AuthenticationStatus::authenticated;
        }
    };

    // Thread-safe in-memory authentication service. Session bearer tokens are
    // returned only once and never retained; the manager stores their SHA-256
    // digests and the independent CSRF secrets.
    class AuthManager {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using LoginCompletion = std::function<void(LoginResult)>;

        explicit AuthManager(AuthOptions options = {});
        ~AuthManager();
        AuthManager(const AuthManager&) = delete;
        AuthManager& operator=(const AuthManager&) = delete;

        static std::string hash_password(std::string_view password,
            const ScryptParameters& parameters = {});
        static bool verify_password(std::string_view password,
            std::string_view encoded_hash,
            std::uint64_t max_memory_bytes =
                64ULL * 1024 * 1024) noexcept;
        /** Validate the encoded format and bounded scrypt cost without hashing. */
        static bool valid_password_hash(std::string_view encoded_hash,
            std::uint64_t max_memory_bytes =
                64ULL * 1024 * 1024) noexcept;

        // Reserves peer and global rate-limit capacity before running scrypt.
        // A successful call returns a 256-bit cookie token and independent
        // 256-bit CSRF token, both encoded as unpadded base64url.
        LoginResult login(std::string_view peer, std::string_view password,
            std::string_view encoded_hash,
            TimePoint now = Clock::now());
        LoginResult login(std::string_view peer, std::string_view password,
            std::string_view encoded_hash, bool principal_matches,
            TimePoint now = Clock::now());

        // Owns all arguments before returning. Exactly one internal crypto
        // worker executes accepted jobs; completion runs on that worker. The
        // caller should post the result back to its network executor.
        LoginSubmission async_login(std::string peer, std::string password,
            std::string encoded_hash, bool principal_matches,
            LoginCompletion completion,
            TimePoint now = Clock::now());

        // Pass csrf_token for state-changing requests. Omitting it authenticates
        // only the bearer cookie. Successful authentication refreshes idle TTL.
        AuthenticationResult authenticate(std::string_view cookie_token,
            std::optional<std::string_view> csrf_token = std::nullopt,
            TimePoint now = Clock::now());
        bool logout(std::string_view cookie_token) noexcept;

        void purge_expired(TimePoint now = Clock::now()) noexcept;
        std::size_t session_count() const noexcept;

    private:
        struct State;
        std::shared_ptr<State> state_;
    };

    std::string_view to_string(LoginStatus status) noexcept;
    std::string_view to_string(LoginSubmissionStatus status) noexcept;
    std::string_view to_string(AuthenticationStatus status) noexcept;

} // namespace circud
