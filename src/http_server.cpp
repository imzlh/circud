#include <circud/http_server.hpp>

#include <circud/auth.hpp>
#include <circud/core.hpp>
#include <circud/endpoint_gateway.hpp>
#include <circud/endpoint_session.hpp>
#include <circud/http_background.hpp>
#include <circud/http_auth.hpp>
#include <circud/http_route.hpp>
#include <circud/rpc_error.hpp>

#include "json.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace circud {
    namespace {

        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        using tcp = asio::ip::tcp;
        using StringResponse = http::response<http::string_body>;

        template <typename Body>
        void set_common_headers(http::response<Body>& response) {
            response.set(http::field::server, "circud/0.1");
            response.set("X-Content-Type-Options", "nosniff");
            response.set("Referrer-Policy", "no-referrer");
            response.set("Content-Security-Policy",
                "default-src 'self'; object-src 'none'; base-uri 'none'; "
                "frame-ancestors 'none'");
        }

        void set_json_error(StringResponse& response, http::status status,
            std::string_view code, std::string_view message) {
            response.result(status);
            response.set(http::field::content_type, "application/json");
            response.set(http::field::cache_control, "no-store");
            response.body() = serialize_json(boost::json::object{
                { "ok", false },
                { "error", boost::json::object{
                    { "code", code }, { "message", message } } } });
        }

        http::status rpc_error_status(std::string_view code) {
            if (code == "not_found" || code == "method_not_found")
                return http::status::not_found;
            if (code == "conflict") return http::status::conflict;
            if (code == "overloaded") return http::status::service_unavailable;
            return http::status::bad_request;
        }

        bool path_within(const std::filesystem::path& path,
            const std::filesystem::path& root) {
            auto path_it = path.begin();
            auto root_it = root.begin();
            for (; root_it != root.end(); ++root_it, ++path_it) {
                if (path_it == path.end() || *path_it != *root_it) return false;
            }
            return true;
        }

        int hex_digit(char value) {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }

        std::filesystem::path decode_target(beast::string_view raw_target) {
            std::string_view target(raw_target.data(), raw_target.size());
            const auto query = target.find_first_of("?#");
            if (query != std::string_view::npos) target = target.substr(0, query);
            if (target.empty() || target.front() != '/')
                throw std::runtime_error("request target must use origin form");

            std::string decoded;
            decoded.reserve(target.size());
            for (std::size_t index = 1; index < target.size(); ++index) {
                const char current = target[index];
                if (current == '%') {
                    if (index + 2 >= target.size())
                        throw std::runtime_error("invalid percent-encoded path");
                    const int high = hex_digit(target[index + 1]);
                    const int low = hex_digit(target[index + 2]);
                    if (high < 0 || low < 0)
                        throw std::runtime_error("invalid percent-encoded path");
                    const char value = static_cast<char>((high << 4) | low);
                    if (value == '\0' || value == '\\' || value == '/')
                        throw std::runtime_error("unsafe encoded path separator");
                    decoded.push_back(value);
                    index += 2;
                }
                else {
                    if (current == '\\' || current == '\0')
                        throw std::runtime_error("unsafe path separator");
                    decoded.push_back(current);
                }
            }
            std::filesystem::path relative(decoded);
            if (relative.is_absolute() || relative.has_root_name())
                throw std::runtime_error("unsafe static path");
            for (const auto& component : relative) {
                if (component == "..")
                    throw std::runtime_error("unsafe static path");
            }
            if (relative.empty() || target.ends_with('/')) relative /= "index.html";
            return relative.lexically_normal();
        }

        std::string content_type(const std::filesystem::path& path) {
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            if (extension == ".html" || extension == ".htm")
                return "text/html; charset=utf-8";
            if (extension == ".css") return "text/css; charset=utf-8";
            if (extension == ".js" || extension == ".mjs")
                return "text/javascript; charset=utf-8";
            if (extension == ".json" || extension == ".map")
                return "application/json; charset=utf-8";
            if (extension == ".svg") return "image/svg+xml";
            if (extension == ".png") return "image/png";
            if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
            if (extension == ".gif") return "image/gif";
            if (extension == ".webp") return "image/webp";
            if (extension == ".ico") return "image/x-icon";
            if (extension == ".woff") return "font/woff";
            if (extension == ".woff2") return "font/woff2";
            if (extension == ".txt") return "text/plain; charset=utf-8";
            return "application/octet-stream";
        }

        std::string make_etag(std::uintmax_t size,
            std::filesystem::file_time_type modified) {
            std::ostringstream output;
            output << '"' << std::hex << size << '-'
                << modified.time_since_epoch().count() << '"';
            return output.str();
        }

        bool media_type_is(beast::string_view value, std::string_view expected) {
            std::string normalized(value.data(), value.size());
            std::ranges::transform(normalized, normalized.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto separator = normalized.find(';');
            auto media_type = std::string_view(normalized).substr(0, separator);
            while (!media_type.empty() &&
                std::isspace(static_cast<unsigned char>(media_type.back())))
                media_type.remove_suffix(1);
            while (!media_type.empty() &&
                std::isspace(static_cast<unsigned char>(media_type.front())))
                media_type.remove_prefix(1);
            return media_type == expected;
        }

        bool is_zip_content_type(beast::string_view value) {
            return media_type_is(value, "application/zip") ||
                media_type_is(value, "application/x-zip-compressed");
        }

        template <typename Body, typename Predicate>
        bool has_one_content_type(const http::request<Body>& request,
            Predicate&& predicate) {
            std::size_t count = 0;
            bool matches = false;
            for (const auto& field : request) {
                if (field.name() != http::field::content_type) continue;
                ++count;
                matches = predicate(field.value());
            }
            return count == 1 && matches;
        }

        template <typename Body>
        bool has_one_expect_100_continue(
            const http::request<Body>& request) {
            std::size_t count = 0;
            bool matches = false;
            for (const auto& field : request) {
                if (field.name() != http::field::expect) continue;
                ++count;
                matches = beast::iequals(field.value(), "100-continue");
            }
            return count == 1 && matches;
        }

        template <typename Body>
        bool has_expect(const http::request<Body>& request) {
            for (const auto& field : request) {
                if (field.name() == http::field::expect) return true;
            }
            return false;
        }

        template <typename Body>
        bool has_one_host(const http::request<Body>& request) {
            std::size_t count = 0;
            for (const auto& field : request) {
                if (field.name() != http::field::host) continue;
                if (++count != 1 || field.value().empty()) return false;
            }
            return count == 1;
        }

        template <typename Body>
        bool has_json_content_type(const http::request<Body>& request) {
            return has_one_content_type(request,
                [](beast::string_view value) {
                    return media_type_is(value, "application/json");
                });
        }

        template <typename Body>
        bool has_zip_content_type(const http::request<Body>& request) {
            return has_one_content_type(request,
                [](beast::string_view value) {
                    return is_zip_content_type(value);
                });
        }

        std::string_view string_view_of(beast::string_view value) noexcept {
            return { value.data(), value.size() };
        }

        AuthOptions auth_options(const AuthConfig& config) {
            AuthOptions options;
            options.max_sessions = config.max_sessions;
            options.idle_timeout = config.session_idle_timeout;
            options.absolute_timeout = config.session_max_lifetime;
            return options;
        }

        struct RequestSession {
            SessionCookieStatus cookie_status = SessionCookieStatus::absent;
            AuthenticationResult authentication;
            std::string token;

            explicit operator bool() const noexcept {
                return static_cast<bool>(authentication);
            }
        };

        template <typename Body>
        SessionCookie request_session_cookie(
            const http::request<Body>& request, std::string& joined) {
            std::size_t fields = 0;
            for (const auto& field : request) {
                if (field.name() != http::field::cookie) continue;
                if (++fields > kMaxHttpCookiePairs) {
                    return { SessionCookieStatus::invalid, {} };
                }
                const auto value = string_view_of(field.value());
                const auto separator = joined.empty() ? 0U : 2U;
                if (value.size() > kMaxHttpCookieBytes -
                        std::min(joined.size() + separator,
                            kMaxHttpCookieBytes)) {
                    return { SessionCookieStatus::invalid, {} };
                }
                if (!joined.empty()) joined += "; ";
                joined.append(value);
            }
            return parse_session_cookie(joined);
        }

        template <typename Body>
        std::optional<std::string_view> request_csrf(
            const http::request<Body>& request, bool& valid) {
            valid = true;
            std::optional<std::string_view> result;
            for (const auto& field : request) {
                if (!beast::iequals(field.name_string(), "X-Circud-CSRF"))
                    continue;
                if (result) {
                    valid = false;
                    return std::nullopt;
                }
                result = string_view_of(field.value());
            }
            return result;
        }

        std::int64_t expiry_millis(std::chrono::milliseconds remaining) {
            const auto now = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch());
            return (now + std::max(remaining,
                std::chrono::milliseconds::zero())).count();
        }

        boost::json::object anonymous_session_json(bool auth_enabled) {
            return {
                { "authEnabled", auth_enabled },
                { "authenticated", false },
            };
        }

        boost::json::object authenticated_session_json(
            std::string_view username, std::string_view csrf,
            std::chrono::milliseconds remaining) {
            return {
                { "authEnabled", true },
                { "authenticated", true },
                { "username", username },
                { "csrfToken", csrf },
                { "expiresAt", expiry_millis(remaining) },
            };
        }

        std::string remote_peer(beast::tcp_stream& stream) {
            boost::system::error_code error;
            const auto endpoint = stream.socket().remote_endpoint(error);
            return error ? "unknown" : endpoint.address().to_string();
        }

        std::chrono::seconds whole_seconds(
            std::chrono::milliseconds value) noexcept {
            const auto milliseconds = std::max<std::int64_t>(
                1, value.count());
            return std::chrono::seconds(milliseconds / 1000 +
                (milliseconds % 1000 == 0 ? 0 : 1));
        }

        std::filesystem::path upload_path(const CoreConfig& config) {
            static std::atomic<std::uint64_t> sequence{ 0 };
            const auto root = config.state_dir / "uploads";
            std::filesystem::create_directories(root);
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                const auto name = ".upload-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "-" + std::to_string(sequence.fetch_add(
                        1, std::memory_order_relaxed)) + ".zip";
                const auto candidate = root / name;
                if (!std::filesystem::exists(candidate)) return candidate;
            }
            throw std::runtime_error("cannot allocate upload file");
        }

        void cleanup_stale_uploads(const CoreConfig& config) {
            const auto root = config.state_dir / "uploads";
            std::error_code error;
            if (!std::filesystem::is_directory(root, error)) return;
            for (std::filesystem::directory_iterator iterator(root, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                const auto name = iterator->path().filename().string();
                if (name.starts_with(".upload-") && name.ends_with(".zip")) {
                    std::error_code ignored;
                    std::filesystem::remove(iterator->path(), ignored);
                }
            }
        }

        boost::json::value installed_package_json(Core& core,
            const StoredPackage& package) {
            auto object = manifest_to_json(package.manifest).as_object();
            object["enabled"] = package.enabled;
            object["runtimeState"] = to_string(
                core.runtime_state(package.manifest.id));
            const auto failure = core.runtime_failure(package.manifest.id);
            if (!failure.empty()) object["runtimeError"] = failure;
            return object;
        }

        StringResponse base_response(unsigned version, bool keep_alive) {
            StringResponse response;
            response.version(version);
            response.keep_alive(keep_alive);
            set_common_headers(response);
            return response;
        }

    } // namespace

    class HttpServer::Impl : public std::enable_shared_from_this<HttpServer::Impl> {
    public:
        class Session;

        struct Generation final {
            Generation(Core& core, std::uint64_t id)
                : id(id), acceptor(core.io()), retry_timer(core.io()) {}

            std::uint64_t id;
            tcp::acceptor acceptor;
            asio::steady_timer retry_timer;
            std::filesystem::path public_root;
            std::shared_ptr<HttpBackground> background;
            std::unordered_set<std::shared_ptr<Session>> sessions;
            std::unordered_set<std::shared_ptr<EndpointSession>> endpoints;
            std::atomic<bool> stopping{ false };
            std::atomic<bool> network_closed{ false };
            std::mutex closed_mutex;
            std::condition_variable closed_changed;
            std::chrono::milliseconds accept_backoff{ 50 };
        };

        explicit Impl(Core& core)
            : core_(core), auth_(auth_options(core.config().auth)) {}

        ~Impl() {
            std::vector<std::shared_ptr<Generation>> generations;
            {
                std::lock_guard lock(start_mutex_);
                if (active_) generations.push_back(std::move(active_));
                generations.insert(generations.end(), retired_.begin(),
                    retired_.end());
                retired_.clear();
            }
            for (const auto& generation : generations) {
                generation->stopping.store(true, std::memory_order_release);
                boost::system::error_code ignored;
                generation->acceptor.close(ignored);
                if (generation->background) generation->background->stop();
            }
            for (const auto& generation : generations)
                if (generation->background) generation->background->join();
        }

        void start() {
            std::unique_lock lock(start_mutex_);
            if (active_) return;
            if (!core_.config().web_enabled)
                throw std::runtime_error(
                    "HTTP server is disabled by configuration");

            const auto old_generations = retired_;
            lock.unlock();
            for (const auto& old : old_generations) {
                if (old->network_closed.load(std::memory_order_acquire))
                    continue;
                if (core_.io().get_executor().running_in_this_thread())
                    throw std::runtime_error(
                        "HTTP listener generation is still stopping");
                std::unique_lock stopped_lock(old->closed_mutex);
                if (!old->closed_changed.wait_for(stopped_lock,
                        std::chrono::seconds(5), [&old] {
                            return old->network_closed.load(
                                std::memory_order_acquire);
                        })) {
                    throw std::runtime_error(
                        "HTTP listener generation did not stop promptly");
                }
            }
            lock.lock();
            if (active_) return;

            std::error_code filesystem_error;
            auto generation = std::make_shared<Generation>(core_,
                ++next_generation_);
            generation->public_root = std::filesystem::canonical(
                core_.config().public_dir, filesystem_error);
            if (filesystem_error || !std::filesystem::is_directory(
                    generation->public_root))
                throw std::runtime_error("public directory is unavailable: " +
                    core_.config().public_dir.string());
            cleanup_stale_uploads(core_.config());

            auto weak_generation =
                std::make_shared<std::weak_ptr<Generation>>(generation);
            generation->background = HttpBackground::create(
                core_.config().http_background_workers,
                [weak_impl = weak_from_this(), weak_generation] {
                    if (const auto owner = weak_impl.lock()) {
                        asio::post(owner->core_.io(),
                            [owner, weak_generation] {
                                if (const auto value = weak_generation->lock())
                                    owner->reap(value);
                            });
                    }
                });
            const auto address = asio::ip::make_address(
                core_.config().listen_address);
            const tcp::endpoint endpoint(address, core_.config().listen_port);
            boost::system::error_code error;
            generation->acceptor.open(endpoint.protocol(), error);
            if (!error) generation->acceptor.set_option(
                tcp::acceptor::reuse_address(true), error);
            if (!error) generation->acceptor.bind(endpoint, error);
            if (!error) generation->acceptor.listen(
                asio::socket_base::max_listen_connections, error);
            if (error) {
                generation->background->stop();
                generation->background->join();
                throw std::runtime_error("cannot start HTTP listener: " +
                    error.message());
            }
            active_ = generation;
            accept(generation);
        }

        void stop();
        void shutdown_and_join() noexcept;

        Core& core() noexcept { return core_; }
        AuthManager& auth() noexcept { return auth_; }
        bool stopping(const std::shared_ptr<Generation>& generation) const
            noexcept {
            return generation->stopping.load(std::memory_order_acquire);
        }

        void forget(const std::shared_ptr<Generation>& generation,
            const std::shared_ptr<Session>& session) {
            generation->sessions.erase(session);
            maybe_stop_background(generation);
        }

        void remember_endpoint(const std::shared_ptr<Generation>& generation,
            const std::shared_ptr<EndpointSession>& session) {
            generation->endpoints.insert(session);
        }

        void forget_endpoint(const std::shared_ptr<Generation>& generation,
            const std::shared_ptr<EndpointSession>& session) {
            generation->endpoints.erase(session);
            maybe_stop_background(generation);
        }

        void complete_endpoint(const std::shared_ptr<Generation>& generation,
            const std::shared_ptr<EndpointSession>& endpoint,
            std::optional<EndpointSession::Handoff> handoff);

        std::size_t connection_count(
            const std::shared_ptr<Generation>& generation) const noexcept {
            return generation->sessions.size() + generation->endpoints.size();
        }

    private:
        static constexpr std::size_t upload_chunk_bytes = 64 * 1024;

        struct UploadState {
            using Parser = http::request_parser<http::buffer_body>;

            std::shared_ptr<Parser> parser;
            std::filesystem::path path;
            std::array<char, upload_chunk_bytes> buffer{};
            std::uintmax_t offset = 0;
            unsigned version = 11;
            bool keep_alive = false;
            std::atomic<bool> finalizing{ false };
        };

        struct StaticTransfer {
            using Response = http::response<http::buffer_body>;
            using Serializer = http::response_serializer<http::buffer_body>;

            std::filesystem::path path;
            std::uintmax_t offset = 0;
            std::uintmax_t remaining = 0;
            std::array<char, upload_chunk_bytes> buffer{};
            std::shared_ptr<Response> response;
            std::shared_ptr<Serializer> serializer;
        };

        void accept(const std::shared_ptr<Generation>& generation);
        void reap(const std::shared_ptr<Generation>& generation);
        void maybe_stop_background(
            const std::shared_ptr<Generation>& generation);

        Core& core_;
        AuthManager auth_;
        std::mutex start_mutex_;
        std::shared_ptr<Generation> active_;
        std::vector<std::shared_ptr<Generation>> retired_;
        std::uint64_t next_generation_ = 0;
    };

    class HttpServer::Impl::Session :
        public std::enable_shared_from_this<HttpServer::Impl::Session> {
    public:
        Session(tcp::socket socket, std::shared_ptr<Impl> server,
            std::shared_ptr<Generation> generation)
            : stream_(std::move(socket)), server_(std::move(server)),
              generation_(std::move(generation)) {}

        Session(EndpointSession::Handoff handoff,
            std::shared_ptr<Impl> server,
            std::shared_ptr<Generation> generation)
            : stream_(std::move(handoff.stream)),
              buffer_(std::move(handoff.buffer)),
              server_(std::move(server)),
              generation_(std::move(generation)) {}

        void run() { read_header(); }

        void close() {
            if (closed_) return;
            closed_ = true;
            boost::system::error_code ignored;
            stream_.socket().cancel(ignored);
            stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
            stream_.socket().close(ignored);
        }

        void shutdown() { finish(); }

    private:
        template <typename Body>
        RequestSession authenticate(const http::request<Body>& request,
            bool require_csrf) {
            RequestSession result;
            if (!server_->core().config().auth.enabled) {
                result.authentication.status =
                    AuthenticationStatus::authenticated;
                return result;
            }

            std::string cookies;
            const auto parsed = request_session_cookie(request, cookies);
            result.cookie_status = parsed.status;
            if (!parsed) return result;
            result.token.assign(parsed.token);

            std::optional<std::string_view> csrf;
            if (require_csrf) {
                bool valid = false;
                const auto supplied = request_csrf(request, valid);
                // Pass an invalid empty token through AuthManager instead of
                // rejecting it here. That lets the manager distinguish an
                // unknown bearer cookie (401) from a valid session with a
                // missing, duplicate, or malformed CSRF header (403).
                csrf = valid && supplied && !supplied->empty()
                    ? supplied
                    : std::optional<std::string_view>(std::string_view{});
            }
            result.authentication = server_->auth().authenticate(
                result.token, csrf);
            return result;
        }

        template <typename Body>
        bool require_authentication(const http::request<Body>& request,
            bool require_csrf, bool close_on_failure = false) {
            const auto session = authenticate(request, require_csrf);
            if (session) return true;
            const bool keep_alive = close_on_failure
                ? false : request.keep_alive();
            if (session.authentication.status ==
                AuthenticationStatus::csrf_mismatch) {
                write_error(request.version(), keep_alive,
                    http::status::forbidden, "csrf_mismatch",
                    "CSRF token is missing or invalid");
            }
            else {
                write_error(request.version(), keep_alive,
                    http::status::unauthorized, "authentication_required",
                    "a valid circud session is required");
            }
            return false;
        }

        void read_header() {
            if (closed_ || server_->stopping(generation_)) return finish();
            current_request_is_head_ = false;
            stream_.expires_after(server_->core().config().http_io_timeout);
            auto parser = std::make_shared<http::request_parser<http::empty_body>>();
            parser->body_limit(std::max({
                server_->core().config().http_max_body_bytes,
                server_->core().config().http_max_upload_bytes,
                server_->core().config().endpoints.max_request_body_bytes }));
            auto self = shared_from_this();
            http::async_read_header(stream_, buffer_, *parser,
                [self, parser](boost::system::error_code error, std::size_t) {
                    self->on_header(error, parser);
                });
        }

        void on_header(boost::system::error_code error,
            const std::shared_ptr<http::request_parser<http::empty_body>>& parser) {
            if (error == http::error::end_of_stream) return finish();
            if (error == http::error::body_limit)
                return write_error(parser->get().version(), false,
                    http::status::payload_too_large, "payload_too_large",
                    "request body exceeds configured limits");
            if (error) return finish();

            current_request_is_head_ =
                parser->get().method() == http::verb::head;

            if (parser->get().version() >= 11 &&
                !has_one_host(parser->get())) {
                return write_error(parser->get().version(), false,
                    http::status::bad_request, "bad_request",
                    "HTTP/1.1 requests require exactly one Host header");
            }

            const auto target = validate_origin_target(string_view_of(
                parser->get().target()));
            if (!target)
                return write_error(parser->get().version(), false,
                    http::status::bad_request, "bad_request",
                    "request target must be a safe origin-form target");
            const auto core_route = route_core_http(string_view_of(
                    parser->get().method_string()),
                target.path);
            if (core_route.status == CoreHttpRouteStatus::not_found)
                return write_error(parser->get().version(), false,
                    http::status::not_found,
                    "not_found", "core API route not found");
            if (core_route.status ==
                CoreHttpRouteStatus::method_not_allowed) {
                auto response = base_response(parser->get().version(),
                    parser->is_done() && parser->get().keep_alive());
                response.set(http::field::allow, core_route.allow);
                set_json_error(response, http::status::method_not_allowed,
                    "method_not_allowed", "method not allowed");
                return write(std::move(response));
            }
            if (core_route.handler == CoreHttpHandler::packages_install) {
                if (!require_authentication(parser->get(), core_route.csrf,
                        true))
                    return;
                return begin_upload(parser);
            }

            if (core_route.handler == CoreHttpHandler::rpc &&
                !require_authentication(parser->get(), core_route.csrf,
                    true)) {
                return;
            }

            if (core_route.status == CoreHttpRouteStatus::matched &&
                core_route.body == CoreHttpBodyPolicy::none &&
                (parser->chunked() ||
                    (parser->content_length() &&
                        *parser->content_length() != 0))) {
                return write_error(parser->get().version(), false,
                    http::status::bad_request, "unexpected_body",
                    "request body is not allowed for this route");
            }

            if (core_route.handler == CoreHttpHandler::auth_login &&
                !server_->core().config().auth.enabled) {
                return write_error(parser->get().version(), false,
                    http::status::conflict, "authentication_disabled",
                    "authentication is disabled");
            }

            if (core_route.body == CoreHttpBodyPolicy::json &&
                !has_json_content_type(parser->get())) {
                return write_error(parser->get().version(), false,
                    http::status::unsupported_media_type,
                    "unsupported_media_type",
                    "Content-Type must be application/json");
            }

            if (has_expect(parser->get())) {
                return write_error(parser->get().version(), false,
                    http::status::expectation_failed,
                    "expectation_failed",
                    "Expect is supported only for package uploads");
            }

            auto body_parser =
                std::make_shared<http::request_parser<http::string_body>>(
                    std::move(*parser));
            const auto body_limit = server_->core().endpoint_gateway().
                    owns_scope(target.path)
                ? server_->core().config().endpoints.max_request_body_bytes
                : server_->core().config().http_max_body_bytes;
            body_parser->body_limit(body_limit);
            if (const auto size = body_parser->content_length();
                size && *size > body_limit) {
                return write_error(body_parser->get().version(), false,
                    http::status::payload_too_large, "payload_too_large",
                    "request body exceeds configured limit");
            }
            stream_.expires_after(server_->core().config().http_io_timeout);
            auto self = shared_from_this();
            http::async_read(stream_, buffer_, *body_parser,
                [self, body_parser](boost::system::error_code read_error,
                    std::size_t) {
                    if (read_error == http::error::body_limit)
                        return self->write_error(
                            body_parser->get().version(), false,
                            http::status::payload_too_large,
                            "payload_too_large",
                            "request body exceeds configured limit");
                    if (read_error) return self->finish();
                    self->dispatch(body_parser->release());
                });
        }

        void begin_upload(
            const std::shared_ptr<http::request_parser<http::empty_body>>& header) {
            const auto version = header->get().version();
            if (!has_zip_content_type(header->get()))
                return write_error(version, false,
                    http::status::unsupported_media_type,
                    "unsupported_media_type",
                    "Content-Type must be application/zip");
            if (const auto size = header->content_length();
                size && *size > server_->core().config().http_max_upload_bytes)
                return write_error(version, false,
                    http::status::payload_too_large, "payload_too_large",
                    "ZIP upload exceeds configured limit");

            if (has_one_expect_100_continue(header->get())) {
                auto interim = std::make_shared<http::response<http::empty_body>>(
                    http::status::continue_, version);
                interim->keep_alive(true);
                stream_.expires_after(server_->core().config().http_io_timeout);
                auto self = shared_from_this();
                http::async_write(stream_, *interim,
                    [self, header, interim](boost::system::error_code error,
                        std::size_t) {
                        if (error) return self->finish();
                        self->read_upload(header);
                    });
                return;
            }
            if (has_expect(header->get())) {
                return write_error(version, false,
                    http::status::expectation_failed,
                    "expectation_failed",
                    "only Expect: 100-continue is supported");
            }
            read_upload(header);
        }

        void read_upload(
            const std::shared_ptr<http::request_parser<http::empty_body>>& header) {
            auto self = shared_from_this();
            auto state = std::make_shared<UploadState>();
            state->version = header->get().version();
            state->keep_alive = header->get().keep_alive();
            active_upload_ = state;
            const bool accepted = generation_->background->post(0,
                [self, state, header, generation = generation_] {
                    try {
                        state->path = upload_path(self->server_->core().config());
                        const auto file = ::open(state->path.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                        if (file >= 0) ::close(file);
                        else state->path.clear();
                    }
                    catch (...) {
                        state->path.clear();
                    }
                    if (state->path.empty() ||
                        generation->stopping.load(std::memory_order_acquire)) {
                        if (!state->path.empty()) {
                            std::error_code ignored;
                            std::filesystem::remove(state->path, ignored);
                        }
                        return asio::post(self->server_->core().io(),
                            [self, state] {
                                self->active_upload_.reset();
                                if (!self->finished_)
                                    self->write_error(state->version, false,
                                        http::status::internal_server_error,
                                        "upload_storage_error",
                                        "cannot create upload temporary file");
                            });
                    }
                    asio::post(self->server_->core().io(),
                        [self, state, header] {
                            if (self->finished_ || self->server_->stopping(
                                    self->generation_))
                                return self->discard_upload(state);
                            state->parser = std::make_shared<UploadState::Parser>(
                                std::move(*header));
                            state->parser->body_limit(self->server_->core().
                                config().http_max_upload_bytes);
                            self->read_upload_chunk(state);
                        });
                });
            if (!accepted) {
                active_upload_.reset();
                write_error(state->version, false,
                    http::status::service_unavailable,
                    "upload_storage_error",
                    "HTTP filesystem queue is unavailable");
            }
        }

        void read_upload_chunk(const std::shared_ptr<UploadState>& state) {
            if (finished_) return discard_upload(state);
            auto& body = state->parser->get().body();
            body.data = state->buffer.data();
            body.size = state->buffer.size();
            stream_.expires_after(server_->core().config().http_io_timeout);
            auto self = shared_from_this();
            http::async_read_some(stream_, buffer_, *state->parser,
                [self, state](boost::system::error_code error, std::size_t) {
                    const auto bytes = state->buffer.size() -
                        state->parser->get().body().size;
                    if (error == http::error::need_buffer) error = {};
                    if (error) {
                        self->discard_upload(state);
                        if (error == http::error::body_limit)
                            return self->write_error(state->version, false,
                                http::status::payload_too_large,
                                "payload_too_large",
                                "ZIP upload exceeds configured limit");
                        return self->finish();
                    }
                    const bool final = state->parser->is_done();
                    if (bytes == 0 && !final)
                        return self->read_upload_chunk(state);
                    self->write_upload_chunk(state, bytes, final);
                });
        }

        void write_upload_chunk(const std::shared_ptr<UploadState>& state,
            std::size_t bytes, bool final) {
            auto self = shared_from_this();
            if (final) state->finalizing.store(true, std::memory_order_release);
            const bool accepted = generation_->background->post(bytes,
                [self, state, bytes, final, generation = generation_] {
                    bool failed = false;
                    std::size_t written = 0;
                    const auto file = ::open(state->path.c_str(),
                        O_WRONLY | O_CLOEXEC);
                    if (file < 0) {
                        failed = true;
                    }
                    else {
                        while (written < bytes) {
                            const auto amount = ::pwrite(file,
                                state->buffer.data() + written,
                                bytes - written,
                                static_cast<off_t>(state->offset + written));
                            if (amount < 0 && errno == EINTR) continue;
                            if (amount <= 0) {
                                failed = true;
                                break;
                            }
                            written += static_cast<std::size_t>(amount);
                        }
                        if (::close(file) != 0) failed = true;
                    }
                    state->offset += written;
                    if (failed || generation->stopping.load(
                            std::memory_order_acquire)) {
                        return self->discard_upload_on_worker(state,
                            failed ? "cannot write upload temporary file" : "");
                    }
                    if (!final) {
                        return asio::post(self->server_->core().io(),
                            [self, state] {
                                if (self->finished_)
                                    return self->discard_upload(state);
                                self->read_upload_chunk(state);
                            });
                    }

                    auto response = base_response(
                        state->version, state->keep_alive);
                    response.set(http::field::cache_control, "no-store");
                    try {
                        const auto package = self->server_->core().install(
                            state->path);
                        response.result(http::status::created);
                        response.set(http::field::content_type,
                            "application/json");
                        response.body() = serialize_json(boost::json::object{
                            { "ok", true },
                            { "result", installed_package_json(
                                self->server_->core(), package) } });
                    }
                    catch (const std::exception& exception) {
                        set_json_error(response, http::status::bad_request,
                            "invalid_package", exception.what());
                    }
                    std::error_code ignored;
                    std::filesystem::remove(state->path, ignored);
                    asio::post(self->server_->core().io(),
                        [self, response = std::move(response)]() mutable {
                            self->active_upload_.reset();
                            self->write(std::move(response));
                        });
                }, [state] { discard_upload_files(state); });
            if (!accepted) {
                state->finalizing.store(false, std::memory_order_release);
                discard_upload(state);
                write_error(state->version, false,
                    http::status::service_unavailable,
                    "upload_storage_error",
                    "HTTP filesystem queue is unavailable");
            }
        }

        static void discard_upload_files(
            const std::shared_ptr<UploadState>& state) noexcept {
            if (!state->path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(state->path, ignored);
            }
        }

        void discard_upload_on_worker(
            const std::shared_ptr<UploadState>& state,
            std::string message) {
            discard_upload_files(state);
            if (message.empty()) return;
            auto self = shared_from_this();
            asio::post(server_->core().io(),
                [self, version = state->version,
                    message = std::move(message)] {
                    self->active_upload_.reset();
                    if (!self->finished_)
                        self->write_error(version, false,
                            http::status::internal_server_error,
                            "upload_storage_error", message);
                });
        }

        void discard_upload(std::shared_ptr<UploadState> state) {
            if (state->finalizing.load(std::memory_order_acquire)) return;
            if (active_upload_ == state) active_upload_.reset();
            if (generation_->background->post(0,
                    [state] { discard_upload_files(state); },
                    [state] { discard_upload_files(state); }))
                return;
            // A stopped executor will drain cancellation tasks that were
            // already admitted. If admission has closed after a socket error,
            // stale uploads are removed on the next listener start.
        }

        void dispatch(http::request<http::string_body> request) {
            const auto target = validate_origin_target(
                string_view_of(request.target()));
            if (!target)
                return write_error(request.version(), false,
                    http::status::bad_request, "bad_request",
                    "request target must be a safe origin-form target");
            const auto core_route = route_core_http(
                string_view_of(request.method_string()),
                target.path);
            if (core_route.status == CoreHttpRouteStatus::not_found)
                return write_error(request.version(), request.keep_alive(),
                    http::status::not_found, "not_found",
                    "core API route not found");
            if (core_route.status ==
                CoreHttpRouteStatus::method_not_allowed) {
                auto response = base_response(
                    request.version(), request.keep_alive());
                response.set(http::field::allow, core_route.allow);
                set_json_error(response, http::status::method_not_allowed,
                    "method_not_allowed", "method not allowed");
                return write(std::move(response));
            }

            if (core_route.handler == CoreHttpHandler::health) {
                auto response = base_response(
                    request.version(), request.keep_alive());
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                response.set(http::field::cache_control, "no-store");
                if (request.method() == http::verb::get)
                    response.body() = R"({"ok":true})";
                else
                    response.content_length(
                        std::string_view(R"({"ok":true})").size());
                return write(std::move(response));
            }

            if (core_route.handler == CoreHttpHandler::auth_session)
                return run_auth_session(std::move(request));
            if (core_route.handler == CoreHttpHandler::auth_login)
                return run_auth_login(std::move(request));
            if (core_route.handler == CoreHttpHandler::auth_logout)
                return run_auth_logout(std::move(request));

            if (core_route.handler == CoreHttpHandler::rpc) {
                if (!has_json_content_type(request))
                    return write_error(request.version(), request.keep_alive(),
                        http::status::unsupported_media_type,
                        "unsupported_media_type",
                        "Content-Type must be application/json");
                return run_rpc(std::move(request));
            }

            // The protocol parser owns the canonical route representation.
            // Preserve the query octets exactly, but do not let package or
            // static routing re-observe a non-canonical spelling such as
            // /ap%69 or /io.example.endpo%69nt.
            std::string canonical_target = target.path;
            if (target.has_query) {
                canonical_target.push_back('?');
                canonical_target.append(target.query);
            }
            request.target(canonical_target);

            if (server_->core().endpoint_gateway().owns_scope(target.path)) {
                const bool authorized = static_cast<bool>(
                    authenticate(request, false));
                return run_endpoint(std::move(request), authorized);
            }

            if (request.method() != http::verb::get &&
                request.method() != http::verb::head) {
                auto response = base_response(
                    request.version(), request.keep_alive());
                response.set(http::field::allow, "GET, HEAD");
                set_json_error(response, http::status::method_not_allowed,
                    "method_not_allowed", "method not allowed");
                return write(std::move(response));
            }

            run_static(std::move(request));
        }

        void run_endpoint(http::request<http::string_body> request,
            bool is_authorized) {
            auto server = server_;
            auto generation = generation_;
            auto slot = std::make_shared<std::weak_ptr<EndpointSession>>();
            auto endpoint = std::make_shared<EndpointSession>(
                server->core(), generation->background,
                std::move(stream_), std::move(buffer_), std::move(request),
                is_authorized,
                [weak_server = std::weak_ptr<Impl>(server), generation, slot](
                    std::optional<EndpointSession::Handoff> handoff) mutable {
                    const auto owner = weak_server.lock();
                    const auto session = slot->lock();
                    if (owner && session)
                        owner->complete_endpoint(generation,
                            session, std::move(handoff));
                });
            *slot = endpoint;
            server->remember_endpoint(generation, endpoint);
            finished_ = true;
            server->forget(generation, shared_from_this());
            endpoint->run();
        }

        void run_auth_session(http::request<http::string_body> request) {
            auto response = base_response(
                request.version(), request.keep_alive());
            response.result(http::status::ok);
            response.set(http::field::content_type, "application/json");
            response.set(http::field::cache_control, "no-store");

            boost::json::object session;
            if (!server_->core().config().auth.enabled) {
                session = anonymous_session_json(false);
            }
            else {
                const auto authenticated = authenticate(request, false);
                if (authenticated) {
                    session = authenticated_session_json(
                        server_->core().config().auth.username,
                        authenticated.authentication.csrf_token,
                        std::min(
                            authenticated.authentication.idle_remaining,
                            authenticated.authentication.absolute_remaining));
                }
                else {
                    session = anonymous_session_json(true);
                }
            }
            response.body() = serialize_json(boost::json::object{
                { "ok", true }, { "result", std::move(session) } });
            write(std::move(response));
        }

        void run_auth_login(http::request<http::string_body> request) {
            if (!server_->core().config().auth.enabled)
                return write_error(request.version(), request.keep_alive(),
                    http::status::conflict, "authentication_disabled",
                    "authentication is disabled");
            if (!has_json_content_type(request))
                return write_error(request.version(), request.keep_alive(),
                    http::status::unsupported_media_type,
                    "unsupported_media_type",
                    "Content-Type must be application/json");

            std::string username;
            std::string password;
            try {
                auto document = parse_json(request.body());
                if (!document.is_object())
                    throw std::runtime_error(
                        "login request must be an object");
                const auto& object = document.as_object();
                if (object.size() != 2 || !object.contains("username") ||
                    !object.contains("password") ||
                    !object.at("username").is_string() ||
                    !object.at("password").is_string()) {
                    throw std::runtime_error(
                        "login requires only username and password strings");
                }
                const auto& supplied_username =
                    object.at("username").as_string();
                const auto& supplied_password =
                    object.at("password").as_string();
                username.assign(supplied_username.data(),
                    supplied_username.size());
                password.assign(supplied_password.data(),
                    supplied_password.size());
            }
            catch (const std::exception& exception) {
                return write_error(request.version(), request.keep_alive(),
                    http::status::bad_request, "bad_request",
                    exception.what());
            }

            const auto version = request.version();
            const auto keep_alive = request.keep_alive();
            const bool principal_matches =
                username == server_->core().config().auth.username;
            auto weak_self = weak_from_this();
            auto weak_server = std::weak_ptr<Impl>(server_);
            const auto submission = server_->auth().async_login(
                remote_peer(stream_), std::move(password),
                server_->core().config().auth.password_hash,
                principal_matches,
                [weak_self, weak_server, version,
                    keep_alive](LoginResult result) mutable {
                    auto self = weak_self.lock();
                    auto server = weak_server.lock();
                    if (!self || !server) return;
                    asio::post(server->core().io(),
                        [self, server, version, keep_alive,
                            result = std::move(result)]() mutable {
                            if (!result) {
                                if (result.status ==
                                    LoginStatus::rate_limited) {
                                    auto response = base_response(
                                        version, keep_alive);
                                    response.set(http::field::retry_after,
                                        std::to_string(whole_seconds(
                                            result.retry_after).count()));
                                    set_json_error(response,
                                        http::status::too_many_requests,
                                        "rate_limited",
                                        "too many login attempts");
                                    response.set(http::field::cache_control,
                                        "no-store");
                                    return self->write(std::move(response));
                                }
                                const auto unavailable =
                                    result.status == LoginStatus::unavailable ||
                                    result.status == LoginStatus::invalid_hash;
                                return self->write_error(version, keep_alive,
                                    unavailable
                                        ? http::status::service_unavailable
                                        : result.status ==
                                                LoginStatus::invalid_request
                                            ? http::status::bad_request
                                            : http::status::unauthorized,
                                    unavailable ? "authentication_unavailable"
                                                : result.status ==
                                                        LoginStatus::invalid_request
                                                    ? "bad_request"
                                                    : "invalid_credentials",
                                    unavailable
                                        ? "authentication is unavailable"
                                        : result.status ==
                                                LoginStatus::invalid_request
                                            ? "invalid login request"
                                            : "invalid username or password");
                            }

                            const auto lifetime = std::min(
                                result.idle_lifetime,
                                result.absolute_lifetime);
                            const auto cookie = build_session_set_cookie(
                                result.token,
                                server->core().config().auth.secure_cookie,
                                whole_seconds(lifetime));
                            if (!cookie)
                                return self->write_error(version, false,
                                    http::status::internal_server_error,
                                    "authentication_unavailable",
                                    "cannot create session cookie");
                            auto response = base_response(version, keep_alive);
                            response.result(http::status::ok);
                            response.set(http::field::content_type,
                                "application/json");
                            response.set(http::field::cache_control,
                                "no-store");
                            response.set(http::field::set_cookie, *cookie);
                            response.body() = serialize_json(
                                boost::json::object{
                                    { "ok", true },
                                    { "result", authenticated_session_json(
                                        server->core().config().auth.username,
                                        result.csrf_token, lifetime) },
                                });
                            self->write(std::move(response));
                        });
                });

            if (submission) return;
            if (submission.status == LoginSubmissionStatus::rate_limited) {
                auto response = base_response(version, keep_alive);
                response.set(http::field::retry_after,
                    std::to_string(whole_seconds(
                        submission.retry_after).count()));
                set_json_error(response, http::status::too_many_requests,
                    "rate_limited", "too many login attempts");
                response.set(http::field::cache_control, "no-store");
                return write(std::move(response));
            }
            const auto bad_request =
                submission.status == LoginSubmissionStatus::invalid_request;
            write_error(version, keep_alive,
                bad_request ? http::status::bad_request
                            : http::status::service_unavailable,
                bad_request ? "bad_request" : "authentication_unavailable",
                bad_request ? "invalid login request"
                            : "authentication is unavailable");
        }

        void run_auth_logout(http::request<http::string_body> request) {
            const auto session = authenticate(request, true);
            if (!session) {
                if (session.authentication.status ==
                    AuthenticationStatus::csrf_mismatch)
                    return write_error(request.version(), request.keep_alive(),
                        http::status::forbidden, "csrf_mismatch",
                        "CSRF token is missing or invalid");
                return write_error(request.version(), request.keep_alive(),
                    http::status::unauthorized, "authentication_required",
                    "a valid circud session is required");
            }
            (void)server_->auth().logout(session.token);
            auto response = base_response(
                request.version(), request.keep_alive());
            response.result(http::status::no_content);
            response.set(http::field::cache_control, "no-store");
            response.set(http::field::set_cookie,
                build_session_clear_cookie(
                    server_->core().config().auth.secure_cookie));
            write(std::move(response));
        }

        void run_rpc(http::request<http::string_body> request) {
            auto self = shared_from_this();
            auto server = server_;
            const auto version = request.version();
            const auto keep_alive = request.keep_alive();
            boost::json::value rpc_request;
            try {
                rpc_request = parse_json(request.body());
            }
            catch (const std::exception& exception) {
                return write_error(version, keep_alive,
                    http::status::bad_request, "bad_request", exception.what());
            }
            server->core().async_handle_rpc(std::move(rpc_request),
                [self, version, keep_alive](RpcOutcome outcome) mutable {
                    auto response = base_response(version, keep_alive);
                    if (auto* failure = std::get_if<RpcFailure>(&outcome)) {
                        set_json_error(response,
                            rpc_error_status(failure->code), failure->code,
                            failure->message);
                    }
                    else {
                        response.result(http::status::ok);
                        response.set(http::field::content_type,
                            "application/json");
                        response.set(http::field::cache_control, "no-store");
                        response.body() = serialize_json(boost::json::object{
                            { "ok", true },
                            { "result", std::get<boost::json::value>(
                                std::move(outcome)) },
                        });
                    }
                    self->write(std::move(response));
                });
        }

        void run_static(http::request<http::string_body> request) {
            auto self = shared_from_this();
            auto server = server_;
            auto generation = generation_;
            const auto version = request.version();
            const auto keep_alive = request.keep_alive();
            const auto method = request.method();
            const auto if_none_match = std::string(
                request[http::field::if_none_match]);
            const auto target = std::string(request.target());
            const bool accepted = generation->background->post(0,
                [self, server, generation, version, keep_alive, method,
                    if_none_match, target]() mutable {
                    auto response = base_response(
                        version, keep_alive);
                    try {
                        const auto relative = decode_target(target);
                        std::error_code file_error;
                        const auto candidate = std::filesystem::canonical(
                            generation->public_root / relative, file_error);
                        if (file_error ||
                            !path_within(candidate, generation->public_root) ||
                            !std::filesystem::is_regular_file(
                                candidate, file_error)) {
                            set_json_error(response, http::status::not_found,
                                "not_found", "static file not found");
                        }
                        else {
                            const auto size = std::filesystem::file_size(
                                candidate, file_error);
                            if (file_error)
                                throw std::runtime_error(
                                    "cannot inspect static file");
                            if (size > server->core().config().
                                http_max_static_file_bytes) {
                                set_json_error(response,
                                    http::status::payload_too_large,
                                    "static_file_too_large",
                                    "static file exceeds configured limit");
                            }
                            else {
                                const auto modified =
                                    std::filesystem::last_write_time(
                                        candidate, file_error);
                                if (file_error)
                                    throw std::runtime_error(
                                        "cannot inspect static file");
                                const auto etag = make_etag(size, modified);
                                response.set(http::field::etag, etag);
                                response.set(http::field::cache_control,
                                    "no-cache");
                                response.set(http::field::content_type,
                                    content_type(candidate));
                                if (if_none_match == etag) {
                                    response.result(http::status::not_modified);
                                }
                                else if (method == http::verb::head) {
                                    response.result(http::status::ok);
                                    response.content_length(size);
                                }
                                else {
                                    auto transfer = std::make_shared<
                                        StaticTransfer>();
                                    const auto file = ::open(candidate.c_str(),
                                        O_RDONLY | O_CLOEXEC);
                                    if (file < 0)
                                        throw std::runtime_error(
                                            "cannot open static file");
                                    ::close(file);
                                    transfer->path = candidate;
                                    transfer->remaining = size;
                                    transfer->response = std::make_shared<
                                        StaticTransfer::Response>(
                                            http::status::ok, version);
                                    transfer->response->keep_alive(keep_alive);
                                    set_common_headers(*transfer->response);
                                    transfer->response->set(http::field::etag,
                                        etag);
                                    transfer->response->set(
                                        http::field::cache_control, "no-cache");
                                    transfer->response->set(
                                        http::field::content_type,
                                        content_type(candidate));
                                    transfer->response->content_length(size);
                                    return asio::post(server->core().io(),
                                        [self, transfer] {
                                            self->read_public_static_chunk(
                                                transfer);
                                        });
                                }
                            }
                        }
                    }
                    catch (const std::exception& exception) {
                        set_json_error(response, http::status::bad_request,
                            "bad_request", exception.what());
                    }
                    asio::post(server->core().io(),
                        [self, response = std::move(response)]() mutable {
                            self->write(std::move(response));
                        });
                });
            if (!accepted)
                write_error(version, false,
                    http::status::service_unavailable,
                    "static_storage_unavailable",
                    "HTTP filesystem queue is unavailable");
        }

        void read_public_static_chunk(
            const std::shared_ptr<StaticTransfer>& transfer) {
            if (finished_) return;
            const bool accepted = generation_->background->post(
                transfer->buffer.size(),
                [self = shared_from_this(), transfer] {
                    const auto wanted = static_cast<std::size_t>(std::min<
                        std::uintmax_t>(transfer->remaining,
                            transfer->buffer.size()));
                    ssize_t amount = 0;
                    const auto file = ::open(transfer->path.c_str(),
                        O_RDONLY | O_CLOEXEC);
                    if (file >= 0) {
                        do {
                            amount = ::pread(file, transfer->buffer.data(),
                                wanted, static_cast<off_t>(transfer->offset));
                        } while (amount < 0 && errno == EINTR);
                        if (::close(file) != 0 && amount >= 0) amount = -1;
                    }
                    else {
                        amount = -1;
                    }
                    if (amount < 0 || (amount == 0 && wanted != 0)) {
                        return asio::post(self->server_->core().io(),
                            [self, transfer] {
                                if (self->finished_) return;
                                if (transfer->serializer)
                                    return self->finish();
                                else
                                    self->write_error(
                                        transfer->response->version(), false,
                                        http::status::bad_request,
                                        "static_storage_error",
                                        "cannot read static file");
                            });
                    }
                    const auto bytes = static_cast<std::size_t>(amount);
                    transfer->offset += bytes;
                    transfer->remaining -= bytes;
                    const bool final = transfer->remaining == 0;
                    asio::post(self->server_->core().io(),
                        [self, transfer, bytes, final] {
                            self->write_public_static_chunk(
                                transfer, bytes, final);
                        });
                });
            if (!accepted)
                write_error(transfer->response->version(), false,
                    http::status::service_unavailable,
                    "static_storage_unavailable",
                    "HTTP filesystem queue is unavailable");
        }

        void write_public_static_chunk(
            const std::shared_ptr<StaticTransfer>& transfer,
            std::size_t bytes, bool final) {
            if (finished_) return;
            auto& body = transfer->response->body();
            body.data = transfer->buffer.data();
            body.size = bytes;
            body.more = !final;
            if (!transfer->serializer)
                transfer->serializer = std::make_shared<
                    StaticTransfer::Serializer>(*transfer->response);
            stream_.expires_after(server_->core().config().http_io_timeout);
            http::async_write(stream_, *transfer->serializer,
                [self = shared_from_this(), transfer, final](
                    boost::system::error_code error, std::size_t) {
                    if (error == http::error::need_buffer && !final)
                        return self->read_public_static_chunk(transfer);
                    if (error || !transfer->response->keep_alive())
                        return self->finish();
                    self->read_header();
                });
        }

        void write_error(unsigned version, bool keep_alive,
            http::status status, std::string_view code,
            std::string_view message) {
            auto response = base_response(version, keep_alive);
            set_json_error(response, status, code, message);
            write(std::move(response));
        }

        void write(StringResponse response) {
            if (closed_ || server_->stopping(generation_)) return finish();

            if (current_request_is_head_) {
                if (!response.body().empty() &&
                    !response.has_content_length()) {
                    response.content_length(response.body().size());
                }
                response.body().clear();
            }
            if (response.result() != http::status::no_content &&
                response.result() != http::status::not_modified &&
                !response.has_content_length()) {
                response.prepare_payload();
            }
            const bool keep_alive = response.keep_alive();
            auto value = std::make_shared<StringResponse>(std::move(response));
            stream_.expires_after(server_->core().config().http_io_timeout);
            auto self = shared_from_this();
            http::async_write(stream_, *value,
                [self, value, keep_alive](boost::system::error_code error,
                    std::size_t) {
                    if (error || !keep_alive) return self->finish();
                    self->read_header();
                });
        }

        void finish() {
            if (finished_) return;
            if (active_upload_) discard_upload(active_upload_);
            finished_ = true;
            close();
            server_->forget(generation_, shared_from_this());
        }

        beast::tcp_stream stream_;
        beast::flat_buffer buffer_;
        std::shared_ptr<Impl> server_;
        std::shared_ptr<Generation> generation_;
        std::shared_ptr<UploadState> active_upload_;
        bool closed_ = false;
        bool finished_ = false;
        bool current_request_is_head_ = false;
    };

    void HttpServer::Impl::complete_endpoint(
        const std::shared_ptr<Generation>& generation,
        const std::shared_ptr<EndpointSession>& endpoint,
        std::optional<EndpointSession::Handoff> handoff) {
        forget_endpoint(generation, endpoint);
        if (!handoff || stopping(generation)) return;

        auto session = std::make_shared<Session>(
            std::move(*handoff), shared_from_this(), generation);
        generation->sessions.insert(session);
        session->run();
    }

    void HttpServer::Impl::stop() {
        std::shared_ptr<Generation> generation;
        {
            std::lock_guard lock(start_mutex_);
            generation = std::move(active_);
            if (generation) retired_.push_back(generation);
        }
        if (!generation) return;
        generation->stopping.store(true, std::memory_order_release);

        auto self = shared_from_this();
        asio::dispatch(core_.io(), [self, generation] {
            boost::system::error_code ignored;
            // Boost 1.90+ removed the cancel(error_code&) overload from
            // waitable timers; the throwing cancel() never fails for a timer
            // on Linux, and the socket/acceptor calls below keep their
            // non-throwing overloads on every supported Boost line.
            generation->retry_timer.cancel();
            generation->acceptor.cancel(ignored);
            generation->acceptor.close(ignored);
            auto sessions = generation->sessions;
            for (const auto& session : sessions) session->shutdown();
            auto endpoint_sessions = generation->endpoints;
            for (const auto& session : endpoint_sessions) session->close();
            generation->network_closed.store(true,
                std::memory_order_release);
            generation->closed_changed.notify_all();
            self->maybe_stop_background(generation);
        });
    }

    void HttpServer::Impl::shutdown_and_join() noexcept {
        try { stop(); }
        catch (...) {}

        std::vector<std::shared_ptr<Generation>> generations;
        {
            std::lock_guard lock(start_mutex_);
            if (active_) generations.push_back(active_);
            generations.insert(generations.end(), retired_.begin(),
                retired_.end());
        }

        for (const auto& generation : generations) {
            while (!generation->network_closed.load(
                    std::memory_order_acquire)) {
                if (core_.io().stopped()) core_.io().restart();
                if (core_.io().poll_one() == 0) {
                    std::unique_lock lock(generation->closed_mutex);
                    generation->closed_changed.wait_for(lock,
                        std::chrono::milliseconds(1));
                }
            }
            generation->background->stop();
        }
        for (const auto& generation : generations)
            generation->background->join();

        // No filesystem worker can post another Core callback after the joins.
        // Drain callbacks already queued by completed tasks while Core is still
        // alive; Core's own conservative destructor handles socket completions.
        for (;;) {
            if (core_.io().stopped()) core_.io().restart();
            if (core_.io().poll() == 0) break;
        }
    }

    void HttpServer::Impl::maybe_stop_background(
        const std::shared_ptr<Generation>& generation) {
        if (!generation->network_closed.load(std::memory_order_acquire) ||
            !generation->sessions.empty() || !generation->endpoints.empty())
            return;
        generation->background->stop();
        reap(generation);
    }

    void HttpServer::Impl::reap(
        const std::shared_ptr<Generation>& generation) {
        if (!generation->background->join_ready()) return;
        generation->background->join();
        std::lock_guard lock(start_mutex_);
        retired_.erase(std::remove(retired_.begin(), retired_.end(), generation),
            retired_.end());
    }

    void HttpServer::Impl::accept(
        const std::shared_ptr<Generation>& generation) {
        if (stopping(generation)) return;
        auto self = shared_from_this();
        generation->acceptor.async_accept(core_.io(),
            [self, generation](boost::system::error_code error,
                tcp::socket socket) {
                if (!error && !self->stopping(generation)) {
                    generation->accept_backoff = std::chrono::milliseconds(50);
                    if (self->connection_count(generation) <
                        self->core_.config().http_max_connections) {
                        auto session = std::make_shared<Session>(
                            std::move(socket), self, generation);
                        generation->sessions.insert(session);
                        session->run();
                    }
                    else {
                        boost::system::error_code ignored;
                        socket.close(ignored);
                    }
                }
                if (self->stopping(generation)) return;
                if (!error) return self->accept(generation);
                if (error == asio::error::operation_aborted) return;
                const auto retry = generation->accept_backoff;
                generation->accept_backoff = std::min(
                    generation->accept_backoff * 2,
                    std::chrono::milliseconds(1000));
                generation->retry_timer.expires_after(retry);
                generation->retry_timer.async_wait(
                    [self, generation](boost::system::error_code timer_error) {
                        if (!timer_error && !self->stopping(generation))
                            self->accept(generation);
                    });
            });
    }

    HttpServer::HttpServer(Core& core) : impl_(std::make_shared<Impl>(core)) {}

    HttpServer::~HttpServer() {
        if (impl_) impl_->shutdown_and_join();
    }

    void HttpServer::start() { impl_->start(); }

    void HttpServer::stop() { impl_->stop(); }

} // namespace circud
