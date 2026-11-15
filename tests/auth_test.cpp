#include "test.hpp"

#include <circud/auth.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

    circud::ScryptParameters test_scrypt() {
        circud::ScryptParameters parameters;
        parameters.log_n = 14;
        parameters.max_memory_bytes = 32ULL * 1024 * 1024;
        return parameters;
    }

    circud::AuthOptions test_options() {
        circud::AuthOptions options;
        options.scrypt = test_scrypt();
        options.idle_timeout = 10s;
        options.absolute_timeout = 30s;
        options.login_window = 10s;
        options.max_login_attempts_per_peer = 2;
        options.max_login_attempts_global = 4;
        return options;
    }

} // namespace

CIRCUD_TEST(auth_scrypt_hash_is_strict_and_verifies_constant_size_digest) {
    const auto hash = circud::AuthManager::hash_password(
        "correct horse battery staple", test_scrypt());
    circud::test::require(hash.starts_with(
        "$circud-scrypt$v=1$ln=14,r=8,p=1$"));
    circud::test::require(circud::AuthManager::verify_password(
        "correct horse battery staple", hash, 32ULL * 1024 * 1024));
    circud::test::require(!circud::AuthManager::verify_password(
        "wrong", hash, 32ULL * 1024 * 1024));
    circud::test::require(!circud::AuthManager::verify_password(
        "correct horse battery staple", hash + "=", 32ULL * 1024 * 1024));
    circud::test::require(!circud::AuthManager::verify_password(
        "correct horse battery staple",
        "$circud-scrypt$v=1$ln=20,r=8,p=1$AAAA$AAAA",
        32ULL * 1024 * 1024));
    circud::test::require(circud::AuthManager::valid_password_hash(
        hash, 32ULL * 1024 * 1024));
    circud::test::require(!circud::AuthManager::valid_password_hash(
        "$argon2id$v=19$not-circud", 32ULL * 1024 * 1024));
}

CIRCUD_TEST(auth_rejects_unbounded_passwords_and_invalid_limits) {
    bool password_rejected = false;
    try {
        (void)circud::AuthManager::hash_password(
            std::string(1025, 'x'), test_scrypt());
    }
    catch (const std::invalid_argument&) {
        password_rejected = true;
    }
    circud::test::require(password_rejected);

    auto options = test_options();
    options.max_sessions = 0;
    bool limits_rejected = false;
    try {
        circud::AuthManager manager(options);
    }
    catch (const std::invalid_argument&) {
        limits_rejected = true;
    }
    circud::test::require(limits_rejected);

    auto insufficient_memory = test_scrypt();
    insufficient_memory.max_memory_bytes = 16ULL * 1024 * 1024;
    bool checked_formula_rejected = false;
    try {
        (void)circud::AuthManager::hash_password("secret",
            insufficient_memory);
    }
    catch (const std::invalid_argument&) {
        checked_formula_rejected = true;
    }
    circud::test::require(checked_formula_rejected,
        "scrypt memory check omitted p and fixed working terms");
}

CIRCUD_TEST(auth_session_requires_valid_token_and_optional_csrf) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    circud::AuthManager manager(test_options());
    const auto start = circud::AuthManager::TimePoint{} + 1h;
    const auto login = manager.login("127.0.0.1", "secret", hash, start);
    circud::test::require(static_cast<bool>(login));
    circud::test::require(login.token.size() == 43);
    circud::test::require(login.csrf_token.size() == 43);
    circud::test::require(login.token != login.csrf_token);
    circud::test::require(manager.session_count() == 1);

    const auto authenticated = manager.authenticate(
        login.token, std::nullopt, start + 5s);
    circud::test::require(static_cast<bool>(authenticated));
    circud::test::require(authenticated.csrf_token == login.csrf_token);
    circud::test::require(authenticated.idle_remaining > 0ms);
    circud::test::require(authenticated.absolute_remaining >
        authenticated.idle_remaining);
    const auto bad_csrf = manager.authenticate(login.token,
        std::string_view("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        start + 6s);
    circud::test::require(bad_csrf.status ==
        circud::AuthenticationStatus::csrf_mismatch);
    circud::test::require(static_cast<bool>(manager.authenticate(login.token,
        login.csrf_token, start + 7s)));
    circud::test::require(manager.logout(login.token));
    circud::test::require(!manager.authenticate(login.token));
    circud::test::require(manager.session_count() == 0);
}

CIRCUD_TEST(auth_session_enforces_idle_and_absolute_ttl) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    circud::AuthManager manager(test_options());
    const auto start = circud::AuthManager::TimePoint{} + 1h;
    const auto idle = manager.login("peer-idle", "secret", hash, start);
    circud::test::require(static_cast<bool>(idle));
    circud::test::require(!manager.authenticate(idle.token, std::nullopt,
        start + 11s));

    const auto fresh = manager.login("peer-absolute", "secret", hash,
        start + 11s);
    circud::test::require(static_cast<bool>(fresh));
    circud::test::require(static_cast<bool>(manager.authenticate(
        fresh.token, std::nullopt, start + 20s)));
    circud::test::require(static_cast<bool>(manager.authenticate(
        fresh.token, std::nullopt, start + 29s)));
    circud::test::require(static_cast<bool>(manager.authenticate(
        fresh.token, std::nullopt, start + 38s)));
    circud::test::require(!manager.authenticate(fresh.token, std::nullopt,
        start + 42s));
}

CIRCUD_TEST(auth_active_peer_table_rejects_new_peer_without_eviction) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    auto options = test_options();
    options.max_tracked_peers = 1;
    options.max_login_attempts_global = 4;
    circud::AuthManager manager(options);
    const auto start = circud::AuthManager::TimePoint{} + 1h;

    const auto first = manager.login("peer-a", "wrong", hash, start);
    circud::test::require(first.status ==
        circud::LoginStatus::invalid_credentials);
    const auto rejected = manager.login("peer-b", "secret", hash, start + 1s);
    circud::test::require(rejected.status == circud::LoginStatus::rate_limited);
    const auto original_still_tracked = manager.login(
        "peer-a", "wrong", hash, start + 2s);
    circud::test::require(original_still_tracked.status ==
        circud::LoginStatus::invalid_credentials);
}

CIRCUD_TEST(auth_async_login_owns_inputs_and_completes_on_crypto_worker) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    auto options = test_options();
    options.max_pending_logins = 2;
    circud::AuthManager manager(options);
    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    circud::LoginResult login;

    std::string peer = "async-peer";
    std::string password = "secret";
    std::string owned_hash = hash;
    const auto submission = manager.async_login(std::move(peer),
        std::move(password), std::move(owned_hash), true,
        [&](circud::LoginResult result) {
            {
                std::lock_guard lock(mutex);
                login = std::move(result);
                completed = true;
            }
            ready.notify_one();
        });
    circud::test::require(static_cast<bool>(submission));
    std::unique_lock lock(mutex);
    circud::test::require(ready.wait_for(lock, 10s,
        [&] { return completed; }), "async login did not complete");
    circud::test::require(static_cast<bool>(login));
    circud::test::require(login.idle_lifetime == options.idle_timeout);
    circud::test::require(login.absolute_lifetime == options.absolute_timeout);
    circud::test::require(login.idle_expires_at < login.absolute_expires_at);
}

CIRCUD_TEST(auth_principal_mismatch_runs_hash_without_creating_session) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    circud::AuthManager manager(test_options());
    const auto start = circud::AuthManager::TimePoint{} + 1h;
    const auto result = manager.login("peer", "secret", hash, false, start);
    circud::test::require(result.status ==
        circud::LoginStatus::invalid_credentials);
    circud::test::require(result.token.empty());
    circud::test::require(manager.session_count() == 0);
}

CIRCUD_TEST(auth_async_principal_mismatch_never_creates_session) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    circud::AuthManager manager(test_options());
    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    circud::LoginResult login;
    const auto submission = manager.async_login("peer", "secret", hash,
        false, [&](circud::LoginResult result) {
            {
                std::lock_guard lock(mutex);
                login = std::move(result);
                completed = true;
            }
            ready.notify_one();
        });
    circud::test::require(static_cast<bool>(submission));
    std::unique_lock lock(mutex);
    circud::test::require(ready.wait_for(lock, 10s,
        [&] { return completed; }));
    circud::test::require(login.status ==
        circud::LoginStatus::invalid_credentials);
    circud::test::require(login.token.empty());
    circud::test::require(manager.session_count() == 0);
}

CIRCUD_TEST(auth_time_inputs_are_monotonic_and_saturating) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    circud::AuthManager saturated_manager(test_options());
    const auto near_max = circud::AuthManager::TimePoint::max() - 1s;
    const auto login = saturated_manager.login(
        "peer-a", "secret", hash, near_max);
    circud::test::require(static_cast<bool>(login));
    circud::test::require(login.idle_expires_at ==
        circud::AuthManager::TimePoint::max());
    circud::test::require(login.absolute_expires_at ==
        circud::AuthManager::TimePoint::max());

    circud::AuthManager monotonic_manager(test_options());
    const auto start = circud::AuthManager::TimePoint{} + 1h;
    const auto normal = monotonic_manager.login(
        "peer-b", "secret", hash, start);
    circud::test::require(static_cast<bool>(normal));
    const auto forward = monotonic_manager.authenticate(normal.token,
        std::nullopt, start + 5s);
    circud::test::require(static_cast<bool>(forward));
    const auto backwards = monotonic_manager.authenticate(normal.token,
        std::nullopt, start + 1s);
    circud::test::require(static_cast<bool>(backwards));
    circud::test::require(backwards.idle_expires_at ==
        forward.idle_expires_at);
}

CIRCUD_TEST(auth_distinguishes_malformed_hash_from_wrong_password) {
    circud::AuthManager manager(test_options());
    const auto start = circud::AuthManager::TimePoint{} + 1h;
    const auto malformed = manager.login("peer-a", "secret", "bad", start);
    circud::test::require(malformed.status == circud::LoginStatus::invalid_hash);

    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    const auto wrong = manager.login("peer-a", "wrong", hash, start + 1s);
    circud::test::require(wrong.status ==
        circud::LoginStatus::invalid_credentials);
}

CIRCUD_TEST(auth_login_rate_limit_is_bounded_per_peer_and_globally) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    auto options = test_options();
    options.max_login_attempts_per_peer = 1;
    options.max_login_attempts_global = 2;
    options.max_tracked_peers = 2;
    circud::AuthManager manager(options);
    const auto start = circud::AuthManager::TimePoint{} + 1h;

    const auto wrong = manager.login("peer-a", "wrong", hash, start);
    circud::test::require(wrong.status ==
        circud::LoginStatus::invalid_credentials);
    const auto peer_limited = manager.login("peer-a", "secret", hash,
        start + 1s);
    circud::test::require(peer_limited.status ==
        circud::LoginStatus::rate_limited);
    circud::test::require(peer_limited.retry_after > 0ms);

    const auto second = manager.login("peer-b", "wrong", hash, start + 1s);
    circud::test::require(second.status ==
        circud::LoginStatus::invalid_credentials);
    const auto global_limited = manager.login("peer-c", "secret", hash,
        start + 2s);
    circud::test::require(global_limited.status ==
        circud::LoginStatus::rate_limited);

    const auto recovered = manager.login("peer-a", "secret", hash,
        start + 11s);
    circud::test::require(static_cast<bool>(recovered));
}

CIRCUD_TEST(auth_max_sessions_evicts_least_recently_used_session) {
    const auto hash = circud::AuthManager::hash_password("secret", test_scrypt());
    auto options = test_options();
    options.max_sessions = 1;
    circud::AuthManager manager(options);
    const auto start = circud::AuthManager::TimePoint{} + 1h;
    const auto first = manager.login("peer-a", "secret", hash, start);
    const auto second = manager.login("peer-b", "secret", hash, start + 1s);
    circud::test::require(static_cast<bool>(first) &&
        static_cast<bool>(second));
    circud::test::require(manager.session_count() == 1);
    circud::test::require(!manager.authenticate(first.token, std::nullopt,
        start + 2s));
    circud::test::require(static_cast<bool>(manager.authenticate(
        second.token, std::nullopt, start + 2s)));
}
