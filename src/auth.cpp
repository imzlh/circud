#include <circud/auth.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace circud {
    namespace {

        constexpr std::size_t kEncodedTokenBytes = 43;
        constexpr std::size_t kMaxPasswordBytes = 1024;
        constexpr std::size_t kMaxEncodedHashBytes = 256;
        constexpr std::size_t kMaxPeerBytes = 256;
        constexpr std::uint64_t kMaxScryptMemory = 256ULL * 1024 * 1024;
        constexpr std::size_t kMaxGlobalScryptInflight = 4;
        constexpr std::string_view kHashPrefix = "$circud-scrypt$v=1$ln=";
        constexpr std::string_view kBase64Url =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

        using Bytes32 = std::array<unsigned char, 32>;

        bool checked_add(std::uint64_t left, std::uint64_t right,
            std::uint64_t& result) noexcept {
            if (right > std::numeric_limits<std::uint64_t>::max() - left)
                return false;
            result = left + right;
            return true;
        }

        bool checked_multiply(std::uint64_t left, std::uint64_t right,
            std::uint64_t& result) noexcept {
            if (left != 0 &&
                right > std::numeric_limits<std::uint64_t>::max() / left)
                return false;
            result = left * right;
            return true;
        }

        bool scrypt_memory_bytes(const ScryptParameters& parameters,
            std::uint64_t& result) noexcept {
            if (parameters.log_n >= 63) return false;
            const auto n = std::uint64_t{ 1 } << parameters.log_n;
            std::uint64_t terms = 0;
            std::uint64_t scaled = 0;
            return checked_add(n, parameters.parallelism, terms) &&
                checked_add(terms, 2, terms) &&
                checked_multiply(128, parameters.block_size, scaled) &&
                checked_multiply(scaled, terms, result);
        }

        struct GlobalCryptoBudget {
            std::mutex mutex;
            std::uint64_t memory_bytes = 0;
            std::size_t inflight = 0;
        };

        GlobalCryptoBudget& global_crypto_budget() {
            static GlobalCryptoBudget budget;
            return budget;
        }

        class GlobalCryptoReservation {
        public:
            explicit GlobalCryptoReservation(std::uint64_t memory_bytes)
                : memory_bytes_(memory_bytes) {
                auto& budget = global_crypto_budget();
                std::lock_guard lock(budget.mutex);
                if (budget.inflight >= kMaxGlobalScryptInflight ||
                    memory_bytes_ > kMaxScryptMemory - budget.memory_bytes)
                    return;
                ++budget.inflight;
                budget.memory_bytes += memory_bytes_;
                held_ = true;
            }

            ~GlobalCryptoReservation() {
                if (!held_) return;
                auto& budget = global_crypto_budget();
                std::lock_guard lock(budget.mutex);
                --budget.inflight;
                budget.memory_bytes -= memory_bytes_;
            }

            explicit operator bool() const noexcept { return held_; }

        private:
            std::uint64_t memory_bytes_ = 0;
            bool held_ = false;
        };

        void validate_scrypt(const ScryptParameters& parameters) {
            if (parameters.log_n < 14 || parameters.log_n > 17 ||
                parameters.block_size == 0 || parameters.block_size > 16 ||
                parameters.parallelism == 0 || parameters.parallelism > 4 ||
                parameters.salt_bytes < 16 || parameters.salt_bytes > 32 ||
                parameters.max_memory_bytes < 16ULL * 1024 * 1024 ||
                parameters.max_memory_bytes > kMaxScryptMemory) {
                throw std::invalid_argument("invalid scrypt parameters");
            }
            std::uint64_t required = 0;
            if (!scrypt_memory_bytes(parameters, required) ||
                required > parameters.max_memory_bytes)
                throw std::invalid_argument("scrypt memory limit is too small");
        }

        void validate_options(const AuthOptions& options) {
            validate_scrypt(options.scrypt);
            if (options.max_sessions == 0 || options.max_sessions > 4096 ||
                options.idle_timeout < std::chrono::seconds(1) ||
                options.absolute_timeout < options.idle_timeout ||
                options.absolute_timeout > std::chrono::hours(24 * 30) ||
                options.login_window < std::chrono::seconds(1) ||
                options.login_window > std::chrono::hours(1) ||
                options.max_login_attempts_per_peer == 0 ||
                options.max_login_attempts_per_peer > 1024 ||
                options.max_login_attempts_global == 0 ||
                options.max_login_attempts_global > 65536 ||
                options.max_login_attempts_per_peer >
                    options.max_login_attempts_global ||
                options.max_tracked_peers == 0 ||
                options.max_tracked_peers > 65536 ||
                options.max_pending_logins == 0 ||
                options.max_pending_logins > 1024 ||
                options.max_scrypt_inflight != 1) {
                throw std::invalid_argument("invalid authentication limits");
            }
        }

        std::string encode_base64url(const unsigned char* data,
            std::size_t size) {
            std::string result;
            result.reserve((size * 4 + 2) / 3);
            std::size_t offset = 0;
            while (offset + 3 <= size) {
                const auto value =
                    (static_cast<unsigned>(data[offset]) << 16) |
                    (static_cast<unsigned>(data[offset + 1]) << 8) |
                    static_cast<unsigned>(data[offset + 2]);
                result.push_back(kBase64Url[(value >> 18) & 63]);
                result.push_back(kBase64Url[(value >> 12) & 63]);
                result.push_back(kBase64Url[(value >> 6) & 63]);
                result.push_back(kBase64Url[value & 63]);
                offset += 3;
            }
            if (offset < size) {
                unsigned value = static_cast<unsigned>(data[offset]) << 16;
                if (offset + 1 < size)
                    value |= static_cast<unsigned>(data[offset + 1]) << 8;
                result.push_back(kBase64Url[(value >> 18) & 63]);
                result.push_back(kBase64Url[(value >> 12) & 63]);
                if (offset + 1 < size)
                    result.push_back(kBase64Url[(value >> 6) & 63]);
            }
            return result;
        }

        int base64url_value(char character) noexcept {
            if (character >= 'A' && character <= 'Z') return character - 'A';
            if (character >= 'a' && character <= 'z')
                return character - 'a' + 26;
            if (character >= '0' && character <= '9')
                return character - '0' + 52;
            if (character == '-') return 62;
            if (character == '_') return 63;
            return -1;
        }

        bool decode_base64url(std::string_view encoded,
            std::vector<unsigned char>& result) {
            if (encoded.empty() || encoded.find('=') != std::string_view::npos ||
                encoded.size() % 4 == 1)
                return false;
            result.clear();
            result.reserve(encoded.size() * 3 / 4);
            std::size_t offset = 0;
            while (offset + 4 <= encoded.size()) {
                unsigned value = 0;
                for (unsigned index = 0; index < 4; ++index) {
                    const auto decoded = base64url_value(encoded[offset + index]);
                    if (decoded < 0) return false;
                    value = (value << 6) | static_cast<unsigned>(decoded);
                }
                result.push_back(static_cast<unsigned char>(value >> 16));
                result.push_back(static_cast<unsigned char>(value >> 8));
                result.push_back(static_cast<unsigned char>(value));
                offset += 4;
            }
            const auto remaining = encoded.size() - offset;
            if (remaining) {
                unsigned value = 0;
                for (std::size_t index = 0; index < remaining; ++index) {
                    const auto decoded = base64url_value(encoded[offset + index]);
                    if (decoded < 0) return false;
                    value = (value << 6) | static_cast<unsigned>(decoded);
                }
                value <<= static_cast<unsigned>((4 - remaining) * 6);
                result.push_back(static_cast<unsigned char>(value >> 16));
                if (remaining == 3)
                    result.push_back(static_cast<unsigned char>(value >> 8));
            }
            return encode_base64url(result.data(), result.size()) == encoded;
        }

        bool parse_unsigned(std::string_view text, unsigned& result) noexcept {
            if (text.empty() || (text.size() > 1 && text.front() == '0'))
                return false;
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), result);
            return parsed.ec == std::errc{} &&
                parsed.ptr == text.data() + text.size();
        }

        struct ParsedHash {
            ScryptParameters parameters;
            std::vector<unsigned char> salt;
            Bytes32 digest{};
        };

        bool parse_hash(std::string_view encoded, std::uint64_t max_memory,
            ParsedHash& result) {
            if (encoded.size() > kMaxEncodedHashBytes ||
                !encoded.starts_with(kHashPrefix) ||
                max_memory < 16ULL * 1024 * 1024 ||
                max_memory > kMaxScryptMemory)
                return false;
            encoded.remove_prefix(kHashPrefix.size());
            const auto r_marker = encoded.find(",r=");
            const auto p_marker = encoded.find(",p=");
            const auto salt_marker = encoded.find('$');
            if (r_marker == std::string_view::npos ||
                p_marker == std::string_view::npos ||
                salt_marker == std::string_view::npos ||
                !(r_marker < p_marker && p_marker < salt_marker))
                return false;
            unsigned log_n = 0;
            unsigned block_size = 0;
            unsigned parallelism = 0;
            if (!parse_unsigned(encoded.substr(0, r_marker), log_n) ||
                !parse_unsigned(encoded.substr(r_marker + 3,
                    p_marker - r_marker - 3), block_size) ||
                !parse_unsigned(encoded.substr(p_marker + 3,
                    salt_marker - p_marker - 3), parallelism))
                return false;
            encoded.remove_prefix(salt_marker + 1);
            const auto digest_marker = encoded.find('$');
            if (digest_marker == std::string_view::npos ||
                encoded.find('$', digest_marker + 1) != std::string_view::npos)
                return false;
            std::vector<unsigned char> digest;
            if (!decode_base64url(encoded.substr(0, digest_marker), result.salt) ||
                !decode_base64url(encoded.substr(digest_marker + 1), digest) ||
                digest.size() != result.digest.size())
                return false;
            std::copy(digest.begin(), digest.end(), result.digest.begin());
            result.parameters.log_n = log_n;
            result.parameters.block_size = block_size;
            result.parameters.parallelism = parallelism;
            result.parameters.salt_bytes = result.salt.size();
            result.parameters.max_memory_bytes = max_memory;
            try {
                validate_scrypt(result.parameters);
            }
            catch (...) {
                return false;
            }
            return true;
        }

        bool derive_scrypt(std::string_view password,
            const ScryptParameters& parameters,
            const unsigned char* salt, std::size_t salt_size,
            unsigned char* output, std::size_t output_size) noexcept {
            const auto n = std::uint64_t{ 1 } << parameters.log_n;
            return EVP_PBE_scrypt(password.data(), password.size(), salt,
                salt_size, n, parameters.block_size, parameters.parallelism,
                parameters.max_memory_bytes, output, output_size) == 1;
        }

        bool sha256(const unsigned char* data, std::size_t size,
            Bytes32& digest) noexcept {
            unsigned digest_size = 0;
            return EVP_Digest(data, size, digest.data(), &digest_size,
                       EVP_sha256(), nullptr) == 1 &&
                digest_size == digest.size();
        }

        bool secure_equal(const unsigned char* left,
            const unsigned char* right, std::size_t size) noexcept {
            return CRYPTO_memcmp(left, right, size) == 0;
        }

        bool decode_token(std::string_view encoded, Bytes32& output) {
            if (encoded.size() != kEncodedTokenBytes) return false;
            std::vector<unsigned char> decoded;
            if (!decode_base64url(encoded, decoded) ||
                decoded.size() != output.size())
                return false;
            std::copy(decoded.begin(), decoded.end(), output.begin());
            return true;
        }

        std::chrono::milliseconds remaining_wait(AuthManager::TimePoint until,
            AuthManager::TimePoint now) noexcept {
            if (until <= now) return std::chrono::milliseconds(1);
            if (until == AuthManager::TimePoint::max())
                return std::chrono::milliseconds::max();
            return std::max(std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    until - now));
        }

        std::chrono::milliseconds remaining_lifetime(
            AuthManager::TimePoint until,
            AuthManager::TimePoint now) noexcept {
            if (until <= now) return std::chrono::milliseconds::zero();
            if (until == AuthManager::TimePoint::max())
                return std::chrono::milliseconds::max();
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                until - now);
        }

        AuthManager::TimePoint saturated_add(AuthManager::TimePoint time,
            AuthManager::Clock::duration duration) noexcept {
            if (duration <= AuthManager::Clock::duration::zero()) return time;
            return time >= AuthManager::TimePoint::max() - duration
                ? AuthManager::TimePoint::max()
                : time + duration;
        }

        AuthManager::TimePoint saturated_sub(AuthManager::TimePoint time,
            AuthManager::Clock::duration duration) noexcept {
            if (duration <= AuthManager::Clock::duration::zero()) return time;
            return time <= AuthManager::TimePoint::min() + duration
                ? AuthManager::TimePoint::min()
                : time - duration;
        }

        enum class VerifyOutcome {
            match,
            mismatch,
            unavailable,
        };

        VerifyOutcome verify_parsed(std::string_view password,
            const ParsedHash& parsed) noexcept {
            std::uint64_t memory_bytes = 0;
            if (!scrypt_memory_bytes(parsed.parameters, memory_bytes))
                return VerifyOutcome::unavailable;
            GlobalCryptoReservation reservation(memory_bytes);
            if (!reservation) return VerifyOutcome::unavailable;
            Bytes32 candidate{};
            const bool derived = derive_scrypt(password, parsed.parameters,
                parsed.salt.data(), parsed.salt.size(), candidate.data(),
                candidate.size());
            const bool matches = derived && secure_equal(candidate.data(),
                parsed.digest.data(), candidate.size());
            OPENSSL_cleanse(candidate.data(), candidate.size());
            if (!derived) return VerifyOutcome::unavailable;
            return matches ? VerifyOutcome::match : VerifyOutcome::mismatch;
        }

    } // namespace

    struct AuthManager::State {
        struct Session {
            Bytes32 token_digest{};
            Bytes32 csrf{};
            TimePoint created_at;
            TimePoint last_seen;
        };

        struct PeerAttempts {
            std::deque<TimePoint> attempts;
            TimePoint last_seen;
        };

        struct LoginJob {
            std::string peer;
            std::string password;
            std::string encoded_hash;
            bool principal_matches = false;
            LoginCompletion completion;
            TimePoint logical_submitted;
            TimePoint actual_submitted;
        };

        explicit State(AuthOptions value) : options(std::move(value)) {}
        ~State() {
            for (auto& session : sessions)
                OPENSSL_cleanse(session.csrf.data(), session.csrf.size());
        }

        bool expired(const Session& session, TimePoint now) const noexcept {
            return now >= saturated_add(session.created_at,
                              options.absolute_timeout) ||
                now >= saturated_add(session.last_seen, options.idle_timeout);
        }

        TimePoint clamp_now(TimePoint proposed) noexcept {
            const auto effective = std::max(proposed, monotonic_now);
            monotonic_now = effective;
            return effective;
        }

        std::chrono::milliseconds peer_capacity_wait(TimePoint now) const {
            auto earliest = TimePoint::max();
            for (const auto& [peer, state] : peer_attempts) {
                (void)peer;
                if (!state.attempts.empty())
                    earliest = std::min(earliest,
                        saturated_add(state.attempts.front(),
                            options.login_window));
            }
            return earliest == TimePoint::max()
                ? std::chrono::milliseconds(1)
                : remaining_wait(earliest, now);
        }

        LoginSubmission reserve_attempt(std::string_view peer,
            TimePoint proposed_now) {
            LoginSubmission result;
            const auto now = clamp_now(proposed_now);
            prune_attempts(now);
            const auto peer_found = peer_attempts.find(std::string(peer));
            const bool peer_limited = peer_found != peer_attempts.end() &&
                peer_found->second.attempts.size() >=
                    options.max_login_attempts_per_peer;
            const bool global_limited = global_attempts.size() >=
                options.max_login_attempts_global;
            if (peer_limited || global_limited) {
                auto wait = std::chrono::milliseconds(1);
                if (peer_limited)
                    wait = std::max(wait, remaining_wait(saturated_add(
                        peer_found->second.attempts.front(),
                        options.login_window), now));
                if (global_limited)
                    wait = std::max(wait, remaining_wait(saturated_add(
                        global_attempts.front(), options.login_window), now));
                result.status = LoginSubmissionStatus::rate_limited;
                result.retry_after = wait;
                return result;
            }
            if (peer_found == peer_attempts.end() &&
                peer_attempts.size() >= options.max_tracked_peers) {
                result.status = LoginSubmissionStatus::rate_limited;
                result.retry_after = peer_capacity_wait(now);
                return result;
            }
            auto& attempts = peer_attempts[std::string(peer)];
            attempts.attempts.push_back(now);
            attempts.last_seen = now;
            global_attempts.push_back(now);
            result.status = LoginSubmissionStatus::accepted;
            return result;
        }

        void create_session(const Bytes32& token_digest, const Bytes32& csrf,
            TimePoint completed, LoginResult& result) {
            purge_sessions(completed);
            if (sessions.size() >= options.max_sessions) {
                const auto oldest = std::min_element(sessions.begin(),
                    sessions.end(), [](const auto& left, const auto& right) {
                        return left.last_seen < right.last_seen;
                    });
                if (oldest != sessions.end()) {
                    OPENSSL_cleanse(oldest->csrf.data(), oldest->csrf.size());
                    sessions.erase(oldest);
                }
            }
            sessions.push_back(
                State::Session{ token_digest, csrf, completed, completed });
            result.status = LoginStatus::authenticated;
            result.idle_lifetime = options.idle_timeout;
            result.absolute_lifetime = options.absolute_timeout;
            result.idle_expires_at = saturated_add(completed,
                options.idle_timeout);
            result.absolute_expires_at = saturated_add(completed,
                options.absolute_timeout);
        }

        void purge_sessions(TimePoint now) {
            for (auto item = sessions.begin(); item != sessions.end();) {
                if (!expired(*item, now)) {
                    ++item;
                    continue;
                }
                OPENSSL_cleanse(item->csrf.data(), item->csrf.size());
                item = sessions.erase(item);
            }
        }

        void prune_attempts(TimePoint now) {
            const auto threshold = saturated_sub(now, options.login_window);
            while (!global_attempts.empty() &&
                global_attempts.front() <= threshold)
                global_attempts.pop_front();
            for (auto item = peer_attempts.begin();
                 item != peer_attempts.end();) {
                while (!item->second.attempts.empty() &&
                    item->second.attempts.front() <= threshold)
                    item->second.attempts.pop_front();
                if (item->second.attempts.empty())
                    item = peer_attempts.erase(item);
                else
                    ++item;
            }
        }

        AuthOptions options;
        mutable std::mutex mutex;
        std::condition_variable worker_cv;
        std::list<Session> sessions;
        std::deque<TimePoint> global_attempts;
        std::unordered_map<std::string, PeerAttempts> peer_attempts;
        std::deque<LoginJob> login_jobs;
        std::thread crypto_worker;
        TimePoint monotonic_now = TimePoint::min();
        std::size_t scrypt_inflight = 0;
        bool stopping = false;
    };

    AuthManager::AuthManager(AuthOptions options) {
        validate_options(options);
        state_ = std::make_shared<State>(std::move(options));
        state_->crypto_worker = std::thread([state = state_] {
            while (true) {
                State::LoginJob job;
                {
                    std::unique_lock lock(state->mutex);
                    state->worker_cv.wait(lock, [&state] {
                        return state->stopping ||
                            (!state->login_jobs.empty() &&
                                state->scrypt_inflight == 0);
                    });
                    if (state->stopping) break;
                    job = std::move(state->login_jobs.front());
                    state->login_jobs.pop_front();
                    ++state->scrypt_inflight;
                }

                LoginResult result;
                ParsedHash parsed;
                if (!parse_hash(job.encoded_hash,
                        state->options.scrypt.max_memory_bytes, parsed)) {
                    result.status = LoginStatus::invalid_hash;
                }
                else {
                    const auto outcome = verify_parsed(job.password, parsed);
                    if (outcome == VerifyOutcome::mismatch ||
                        !job.principal_matches) {
                        result.status = LoginStatus::invalid_credentials;
                    }
                    else if (outcome == VerifyOutcome::unavailable) {
                        result.status = LoginStatus::unavailable;
                    }
                    else {
                        Bytes32 token{};
                        Bytes32 csrf{};
                        Bytes32 token_digest{};
                        if (RAND_bytes(token.data(),
                                static_cast<int>(token.size())) != 1 ||
                            RAND_bytes(csrf.data(),
                                static_cast<int>(csrf.size())) != 1 ||
                            !sha256(token.data(), token.size(), token_digest)) {
                            result.status = LoginStatus::unavailable;
                        }
                        else {
                            result.token = encode_base64url(
                                token.data(), token.size());
                            result.csrf_token = encode_base64url(
                                csrf.data(), csrf.size());
                            std::lock_guard lock(state->mutex);
                            const auto elapsed = Clock::now() -
                                job.actual_submitted;
                            const auto completed = state->clamp_now(
                                saturated_add(job.logical_submitted, elapsed));
                            if (state->stopping) {
                                result.token.clear();
                                result.csrf_token.clear();
                                result.status = LoginStatus::unavailable;
                            }
                            else {
                                state->create_session(token_digest, csrf,
                                    completed, result);
                            }
                        }
                        OPENSSL_cleanse(token.data(), token.size());
                        OPENSSL_cleanse(csrf.data(), csrf.size());
                    }
                }
                OPENSSL_cleanse(job.password.data(), job.password.size());
                {
                    std::lock_guard lock(state->mutex);
                    --state->scrypt_inflight;
                }
                state->worker_cv.notify_all();
                try {
                    job.completion(std::move(result));
                }
                catch (...) {}
            }
        });
    }

    AuthManager::~AuthManager() {
        // Destruction from a completion callback runs on the crypto worker.
        // State is shared with that worker, so detaching in this one case is
        // lifetime-safe; ordinary destruction joins before returning.
        std::deque<State::LoginJob> cancelled;
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
            cancelled.swap(state_->login_jobs);
        }
        state_->worker_cv.notify_all();
        if (state_->crypto_worker.joinable()) {
            if (state_->crypto_worker.get_id() == std::this_thread::get_id()) {
                state_->crypto_worker.detach();
            }
            else {
                state_->crypto_worker.join();
            }
        }
        for (auto& job : cancelled) {
            OPENSSL_cleanse(job.password.data(), job.password.size());
            LoginResult result;
            result.status = LoginStatus::unavailable;
            try {
                job.completion(std::move(result));
            }
            catch (...) {}
        }
    }

    std::string AuthManager::hash_password(std::string_view password,
        const ScryptParameters& parameters) {
        validate_scrypt(parameters);
        if (password.empty() || password.size() > kMaxPasswordBytes)
            throw std::invalid_argument("password must contain 1..1024 bytes");
        std::uint64_t memory_bytes = 0;
        if (!scrypt_memory_bytes(parameters, memory_bytes))
            throw std::invalid_argument("invalid scrypt memory requirement");
        GlobalCryptoReservation reservation(memory_bytes);
        if (!reservation)
            throw std::runtime_error("scrypt capacity is unavailable");
        std::vector<unsigned char> salt(parameters.salt_bytes);
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
            throw std::runtime_error("secure random generation failed");
        Bytes32 digest{};
        if (!derive_scrypt(password, parameters, salt.data(), salt.size(),
                digest.data(), digest.size())) {
            OPENSSL_cleanse(digest.data(), digest.size());
            throw std::runtime_error("scrypt password hashing failed");
        }
        auto result = std::string(kHashPrefix) +
            std::to_string(parameters.log_n) + ",r=" +
            std::to_string(parameters.block_size) + ",p=" +
            std::to_string(parameters.parallelism) + "$" +
            encode_base64url(salt.data(), salt.size()) + "$" +
            encode_base64url(digest.data(), digest.size());
        OPENSSL_cleanse(digest.data(), digest.size());
        return result;
    }

    bool AuthManager::verify_password(std::string_view password,
        std::string_view encoded_hash,
        std::uint64_t max_memory_bytes) noexcept {
        try {
            if (password.empty() || password.size() > kMaxPasswordBytes)
                return false;
            ParsedHash parsed;
            if (!parse_hash(encoded_hash, max_memory_bytes, parsed)) return false;
            return verify_parsed(password, parsed) == VerifyOutcome::match;
        }
        catch (...) {
            return false;
        }
    }

    bool AuthManager::valid_password_hash(std::string_view encoded_hash,
        std::uint64_t max_memory_bytes) noexcept {
        try {
            ParsedHash parsed;
            return parse_hash(encoded_hash, max_memory_bytes, parsed);
        }
        catch (...) {
            return false;
        }
    }

    LoginResult AuthManager::login(std::string_view peer,
        std::string_view password, std::string_view encoded_hash,
        TimePoint now) {
        return login(peer, password, encoded_hash, true, now);
    }

    LoginResult AuthManager::login(std::string_view peer,
        std::string_view password, std::string_view encoded_hash,
        bool principal_matches, TimePoint now) {
        LoginResult result;
        if (peer.empty() || peer.size() > kMaxPeerBytes || password.empty() ||
            password.size() > kMaxPasswordBytes ||
            encoded_hash.size() > kMaxEncodedHashBytes) {
            result.status = LoginStatus::invalid_request;
            return result;
        }

        ParsedHash parsed;
        if (!parse_hash(encoded_hash,
                state_->options.scrypt.max_memory_bytes, parsed)) {
            result.status = LoginStatus::invalid_hash;
            return result;
        }
        TimePoint logical_started;
        const auto actual_started = Clock::now();
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopping) return result;
            if (state_->scrypt_inflight != 0) {
                result.status = LoginStatus::unavailable;
                return result;
            }
            const auto submission = state_->reserve_attempt(peer, now);
            if (!submission) {
                result.status = submission.status ==
                        LoginSubmissionStatus::rate_limited
                    ? LoginStatus::rate_limited
                    : LoginStatus::unavailable;
                result.retry_after = submission.retry_after;
                return result;
            }
            ++state_->scrypt_inflight;
            logical_started = state_->monotonic_now;
        }

        const auto outcome = verify_parsed(password, parsed);
        {
            std::lock_guard lock(state_->mutex);
            --state_->scrypt_inflight;
        }
        state_->worker_cv.notify_all();
        if (outcome == VerifyOutcome::mismatch || !principal_matches) {
            result.status = LoginStatus::invalid_credentials;
            return result;
        }
        if (outcome == VerifyOutcome::unavailable) return result;

        Bytes32 token{};
        Bytes32 csrf{};
        if (RAND_bytes(token.data(), static_cast<int>(token.size())) != 1 ||
            RAND_bytes(csrf.data(), static_cast<int>(csrf.size())) != 1) {
            OPENSSL_cleanse(token.data(), token.size());
            OPENSSL_cleanse(csrf.data(), csrf.size());
            result.status = LoginStatus::unavailable;
            return result;
        }
        Bytes32 token_digest{};
        if (!sha256(token.data(), token.size(), token_digest)) {
            OPENSSL_cleanse(token.data(), token.size());
            OPENSSL_cleanse(csrf.data(), csrf.size());
            result.status = LoginStatus::unavailable;
            return result;
        }

        result.token = encode_base64url(token.data(), token.size());
        result.csrf_token = encode_base64url(csrf.data(), csrf.size());
        {
            std::lock_guard lock(state_->mutex);
            const auto completed = state_->clamp_now(saturated_add(
                logical_started, Clock::now() - actual_started));
            if (state_->stopping) {
                result.token.clear();
                result.csrf_token.clear();
                result.status = LoginStatus::unavailable;
            }
            else {
                state_->create_session(token_digest, csrf, completed, result);
            }
        }
        OPENSSL_cleanse(token.data(), token.size());
        OPENSSL_cleanse(csrf.data(), csrf.size());
        return result;
    }

    LoginSubmission AuthManager::async_login(std::string peer,
        std::string password, std::string encoded_hash,
        bool principal_matches, LoginCompletion completion, TimePoint now) {
        LoginSubmission result;
        if (!completion || peer.empty() || peer.size() > kMaxPeerBytes ||
            password.empty() || password.size() > kMaxPasswordBytes ||
            encoded_hash.size() > kMaxEncodedHashBytes) {
            result.status = LoginSubmissionStatus::invalid_request;
            return result;
        }
        ParsedHash parsed;
        if (!parse_hash(encoded_hash,
                state_->options.scrypt.max_memory_bytes, parsed)) {
            result.status = LoginSubmissionStatus::invalid_hash;
            return result;
        }
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopping) {
                result.status = LoginSubmissionStatus::stopping;
                return result;
            }
            if (state_->login_jobs.size() >=
                state_->options.max_pending_logins) {
                result.status = LoginSubmissionStatus::overloaded;
                return result;
            }
            result = state_->reserve_attempt(peer, now);
            if (!result) return result;
            state_->login_jobs.push_back(State::LoginJob{
                std::move(peer), std::move(password),
                std::move(encoded_hash), principal_matches,
                std::move(completion),
                state_->monotonic_now, Clock::now() });
        }
        state_->worker_cv.notify_one();
        return result;
    }

    AuthenticationResult AuthManager::authenticate(
        std::string_view cookie_token,
        std::optional<std::string_view> csrf_token, TimePoint now) {
        AuthenticationResult result;
        Bytes32 token{};
        Bytes32 token_digest{};
        if (!decode_token(cookie_token, token) ||
            !sha256(token.data(), token.size(), token_digest))
            return result;
        OPENSSL_cleanse(token.data(), token.size());

        Bytes32 supplied_csrf{};
        const bool csrf_valid = !csrf_token ||
            decode_token(*csrf_token, supplied_csrf);
        std::lock_guard lock(state_->mutex);
        now = state_->clamp_now(now);
        state_->purge_sessions(now);
        State::Session* matched = nullptr;
        for (auto& session : state_->sessions) {
            if (secure_equal(session.token_digest.data(), token_digest.data(),
                    token_digest.size()))
                matched = &session;
        }
        if (!matched) return result;
        const bool csrf_matches = secure_equal(matched->csrf.data(),
            supplied_csrf.data(), supplied_csrf.size());
        if (csrf_token && (!csrf_valid || !csrf_matches)) {
            result.status = AuthenticationStatus::csrf_mismatch;
            return result;
        }
        matched->last_seen = now;
        result.status = AuthenticationStatus::authenticated;
        result.csrf_token = encode_base64url(
            matched->csrf.data(), matched->csrf.size());
        result.idle_expires_at = saturated_add(matched->last_seen,
            state_->options.idle_timeout);
        result.absolute_expires_at = saturated_add(matched->created_at,
            state_->options.absolute_timeout);
        result.idle_remaining = remaining_lifetime(
            result.idle_expires_at, now);
        result.absolute_remaining = remaining_lifetime(
            result.absolute_expires_at, now);
        return result;
    }

    bool AuthManager::logout(std::string_view cookie_token) noexcept {
        try {
            Bytes32 token{};
            Bytes32 digest{};
            if (!decode_token(cookie_token, token) ||
                !sha256(token.data(), token.size(), digest))
                return false;
            OPENSSL_cleanse(token.data(), token.size());
            std::lock_guard lock(state_->mutex);
            bool removed = false;
            for (auto item = state_->sessions.begin();
                 item != state_->sessions.end();) {
                if (secure_equal(item->token_digest.data(), digest.data(),
                        digest.size())) {
                    OPENSSL_cleanse(item->csrf.data(), item->csrf.size());
                    item = state_->sessions.erase(item);
                    removed = true;
                }
                else {
                    ++item;
                }
            }
            return removed;
        }
        catch (...) {
            return false;
        }
    }

    void AuthManager::purge_expired(TimePoint now) noexcept {
        try {
            std::lock_guard lock(state_->mutex);
            now = state_->clamp_now(now);
            state_->purge_sessions(now);
            state_->prune_attempts(now);
        }
        catch (...) {}
    }

    std::size_t AuthManager::session_count() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->sessions.size();
    }

    std::string_view to_string(LoginStatus status) noexcept {
        switch (status) {
        case LoginStatus::authenticated: return "authenticated";
        case LoginStatus::invalid_credentials: return "invalid_credentials";
        case LoginStatus::rate_limited: return "rate_limited";
        case LoginStatus::invalid_request: return "invalid_request";
        case LoginStatus::invalid_hash: return "invalid_hash";
        case LoginStatus::unavailable: return "unavailable";
        }
        return "unknown";
    }

    std::string_view to_string(LoginSubmissionStatus status) noexcept {
        switch (status) {
        case LoginSubmissionStatus::accepted: return "accepted";
        case LoginSubmissionStatus::invalid_request: return "invalid_request";
        case LoginSubmissionStatus::invalid_hash: return "invalid_hash";
        case LoginSubmissionStatus::rate_limited: return "rate_limited";
        case LoginSubmissionStatus::overloaded: return "overloaded";
        case LoginSubmissionStatus::stopping: return "stopping";
        }
        return "unknown";
    }

    std::string_view to_string(AuthenticationStatus status) noexcept {
        switch (status) {
        case AuthenticationStatus::authenticated: return "authenticated";
        case AuthenticationStatus::invalid_token: return "invalid_token";
        case AuthenticationStatus::csrf_mismatch: return "csrf_mismatch";
        }
        return "unknown";
    }

} // namespace circud
