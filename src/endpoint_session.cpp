#include <circud/endpoint_session.hpp>

#include <circud/core.hpp>
#include <circud/endpoint_gateway.hpp>
#include <circud/http_auth.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace circud {
    namespace {

        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace websocket = beast::websocket;

        using StringRequest = http::request<http::string_body>;
        using StringResponse = http::response<http::string_body>;

        std::size_t saturated_add(std::size_t left,
            std::size_t right) noexcept {
            if (right > std::numeric_limits<std::size_t>::max() - left)
                return std::numeric_limits<std::size_t>::max();
            return left + right;
        }

        std::size_t header_bytes(const EndpointHeaders& headers) noexcept {
            std::size_t bytes = 0;
            for (const auto& [name, value] : headers) {
                bytes = saturated_add(bytes, name.size());
                bytes = saturated_add(bytes, value.size());
            }
            return bytes;
        }

        std::size_t output_bytes(const EndpointOutput& output) noexcept {
            return std::visit([](const auto& value) -> std::size_t {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, EndpointResponse>)
                    return saturated_add(value.body.size(),
                        header_bytes(value.headers));
                else if constexpr (std::is_same_v<Value, EndpointAccept>)
                    return saturated_add(value.subprotocol.size(),
                        header_bytes(value.headers));
                else if constexpr (std::is_same_v<Value, EndpointStatic>)
                    return value.relative_path.size();
                else if constexpr (std::is_same_v<Value, EndpointMessage>)
                    return value.data.size();
                else
                    return value.reason.size();
            }, output);
        }

        bool terminal_output(const EndpointOutput& output) noexcept {
            return std::holds_alternative<EndpointResponse>(output) ||
                std::holds_alternative<EndpointStatic>(output) ||
                std::holds_alternative<EndpointClose>(output);
        }

        std::string lower_ascii(std::string_view value) {
            std::string lowered(value);
            std::ranges::transform(lowered, lowered.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return lowered;
        }

        bool token_character(unsigned char character) noexcept {
            if (std::isalnum(character)) return true;
            constexpr std::string_view punctuation =
                "!#$%&'*+-.^_`|~";
            return punctuation.find(static_cast<char>(character)) !=
                std::string_view::npos;
        }

        bool valid_token(std::string_view value) noexcept {
            return !value.empty() &&
                std::ranges::all_of(value, [](unsigned char character) {
                    return token_character(character);
                });
        }

        bool valid_header_value(std::string_view value) noexcept {
            return std::ranges::all_of(value, [](unsigned char character) {
                return character != '\r' && character != '\n' &&
                    character != '\0' &&
                    (character == '\t' || character >= 0x20);
            });
        }

        bool media_type_is(std::string_view value,
            std::string_view expected) {
            const auto separator = value.find(';');
            auto media_type = value.substr(0, separator);
            while (!media_type.empty() &&
                std::isspace(static_cast<unsigned char>(media_type.front())))
                media_type.remove_prefix(1);
            while (!media_type.empty() &&
                std::isspace(static_cast<unsigned char>(media_type.back())))
                media_type.remove_suffix(1);
            return lower_ascii(media_type) == expected;
        }

        bool forbidden_response_header(std::string_view name,
            std::string_view value, bool websocket_response) {
            const auto lowered = lower_ascii(name);
            if (lowered == "connection" || lowered == "keep-alive" ||
                lowered == "proxy-connection" || lowered == "te" ||
                lowered == "trailer" || lowered == "transfer-encoding" ||
                lowered == "upgrade" || lowered == "content-length")
                return true;
            if (lowered == "set-cookie") {
                const auto separator = value.find('=');
                auto cookie_name = value.substr(0, separator);
                while (!cookie_name.empty() &&
                    std::isspace(static_cast<unsigned char>(
                        cookie_name.front())))
                    cookie_name.remove_prefix(1);
                while (!cookie_name.empty() &&
                    std::isspace(static_cast<unsigned char>(
                        cookie_name.back())))
                    cookie_name.remove_suffix(1);
                if (lower_ascii(cookie_name) == "circud_session") return true;
            }
            return websocket_response && lowered.starts_with("sec-websocket-");
        }

        bool validate_headers(const EndpointHeaders& headers,
            bool websocket_response = false) noexcept {
            if (header_bytes(headers) > kMaxEndpointEnvelopeBytes) return false;
            return std::ranges::all_of(headers,
                [websocket_response](const auto& header) {
                    return valid_token(header.first) &&
                        valid_header_value(header.second) &&
                        !forbidden_response_header(
                            header.first, header.second, websocket_response);
                });
        }

        bool response_status_forbids_body(unsigned status,
            http::verb request_method) noexcept {
            return status < 200 || status == 204 || status == 205 ||
                status == 304 ||
                (request_method == http::verb::connect && status < 300);
        }

        bool path_within(const std::filesystem::path& path,
            const std::filesystem::path& root) {
            auto path_part = path.begin();
            for (auto root_part = root.begin(); root_part != root.end();
                 ++root_part, ++path_part) {
                if (path_part == path.end() || *path_part != *root_part)
                    return false;
            }
            return true;
        }

        bool safe_relative_path(const std::filesystem::path& path) {
            if (path.empty() || path.is_absolute() || path.has_root_name() ||
                path.has_root_directory())
                return false;
            return std::ranges::none_of(path,
                [](const auto& component) { return component == ".."; });
        }

        std::string content_type(const std::filesystem::path& path) {
            const auto extension = lower_ascii(path.extension().string());
            if (extension == ".html" || extension == ".htm")
                return "text/html; charset=utf-8";
            if (extension == ".css") return "text/css; charset=utf-8";
            if (extension == ".js" || extension == ".mjs")
                return "text/javascript; charset=utf-8";
            if (extension == ".json" || extension == ".map")
                return "application/json; charset=utf-8";
            if (extension == ".svg") return "image/svg+xml";
            if (extension == ".png") return "image/png";
            if (extension == ".jpg" || extension == ".jpeg")
                return "image/jpeg";
            if (extension == ".gif") return "image/gif";
            if (extension == ".webp") return "image/webp";
            if (extension == ".ico") return "image/x-icon";
            if (extension == ".woff") return "font/woff";
            if (extension == ".woff2") return "font/woff2";
            if (extension == ".txt") return "text/plain; charset=utf-8";
            return "application/octet-stream";
        }

        bool valid_utf8(const EndpointBytes& bytes) noexcept {
            std::size_t index = 0;
            while (index < bytes.size()) {
                const auto first = std::to_integer<unsigned char>(bytes[index]);
                std::size_t following = 0;
                unsigned minimum = 0;
                unsigned value = 0;
                if (first < 0x80) {
                    ++index;
                    continue;
                }
                if ((first & 0xe0) == 0xc0) {
                    following = 1;
                    value = first & 0x1f;
                    minimum = 0x80;
                }
                else if ((first & 0xf0) == 0xe0) {
                    following = 2;
                    value = first & 0x0f;
                    minimum = 0x800;
                }
                else if ((first & 0xf8) == 0xf0) {
                    following = 3;
                    value = first & 0x07;
                    minimum = 0x10000;
                }
                else {
                    return false;
                }
                if (following > bytes.size() - index - 1) return false;
                for (std::size_t offset = 1; offset <= following; ++offset) {
                    const auto next =
                        std::to_integer<unsigned char>(bytes[index + offset]);
                    if ((next & 0xc0) != 0x80) return false;
                    value = (value << 6) | (next & 0x3f);
                }
                if (value < minimum || value > 0x10ffff ||
                    (value >= 0xd800 && value <= 0xdfff))
                    return false;
                index += following + 1;
            }
            return true;
        }

        std::vector<std::string> offered_subprotocols(
            beast::string_view header) {
            std::vector<std::string> result;
            std::string_view remaining(header.data(), header.size());
            while (!remaining.empty()) {
                const auto comma = remaining.find(',');
                auto item = remaining.substr(0, comma);
                while (!item.empty() &&
                    std::isspace(static_cast<unsigned char>(item.front())))
                    item.remove_prefix(1);
                while (!item.empty() &&
                    std::isspace(static_cast<unsigned char>(item.back())))
                    item.remove_suffix(1);
                if (!valid_token(item)) return {};
                result.emplace_back(item);
                if (comma == std::string_view::npos) break;
                remaining.remove_prefix(comma + 1);
            }
            return result;
        }

        EndpointBytes copy_bytes(std::string_view value) {
            EndpointBytes bytes(value.size());
            if (!value.empty())
                std::memcpy(bytes.data(), value.data(), value.size());
            return bytes;
        }

        http::status gateway_status(EndpointOpenStatus status) noexcept {
            switch (status) {
            case EndpointOpenStatus::invalid_request:
                return http::status::bad_request;
            case EndpointOpenStatus::not_found:
                return http::status::not_found;
            case EndpointOpenStatus::disabled:
            case EndpointOpenStatus::unavailable:
            case EndpointOpenStatus::overloaded:
                return http::status::service_unavailable;
            case EndpointOpenStatus::accepted:
                return http::status::ok;
            }
            return http::status::internal_server_error;
        }

        bool valid_websocket_close_code(std::uint16_t code) noexcept {
            if (code < 1000 || code >= 5000) return false;
            if (code >= 3000) return true;
            return code == 1000 || code == 1001 || code == 1002 ||
                code == 1003 || (code >= 1007 && code <= 1013);
        }

    } // namespace

    class EndpointSession::State final :
        public std::enable_shared_from_this<EndpointSession::State> {
    public:
        State(Core& core, std::shared_ptr<HttpBackground> background,
            beast::tcp_stream&& stream,
            beast::flat_buffer&& buffer, StringRequest&& request,
            bool is_authorized,
            Completion completion)
            : core_(core), strand_(asio::make_strand(core.io())),
              idle_timer_(strand_), lifetime_timer_(strand_),
              background_(std::move(background)),
              stream_(std::move(stream)), buffer_(std::move(buffer)),
              request_(std::move(request)), is_authorized_(is_authorized),
              completion_(std::move(completion)) {}

        ~State() {
            close_socket();
        }

        void run(std::shared_ptr<EndpointSink> sink) {
            auto self = shared_from_this();
            asio::post(strand_,
                [self, sink = std::move(sink)]() mutable {
                    self->open(std::move(sink));
                });
        }

        void request_close() {
            auto self = shared_from_this();
            asio::dispatch(strand_, [self] {
                self->finish(true, 1001, "network session closed");
            });
        }

        bool emit(EndpointOutput output) {
            const auto bytes = output_bytes(output);
            const bool terminal = terminal_output(output);
            const bool response =
                std::holds_alternative<EndpointResponse>(output);
            bool overflow = false;
            {
                std::lock_guard lock(queue_mutex_);
                const auto& limits = core_.config().endpoints;
                const auto byte_limit = response
                    ? std::max(limits.max_queued_bytes,
                        saturated_add(limits.max_response_body_bytes,
                            kMaxEndpointEnvelopeBytes))
                    : limits.max_queued_bytes;
                if (!accepting_outputs_) return false;
                if (queued_messages_ >= limits.max_queued_messages ||
                    bytes > byte_limit ||
                    queued_bytes_ > byte_limit - bytes) {
                    accepting_outputs_ = false;
                    overflow = true;
                }
                else {
                    queue_.push_back(QueuedOutput{ std::move(output), bytes });
                    ++queued_messages_;
                    queued_bytes_ += bytes;
                    if (terminal) accepting_outputs_ = false;
                }
            }
            auto self = shared_from_this();
            if (overflow) {
                asio::post(strand_, [self] {
                    self->finish(true, 1011,
                        "package endpoint output queue overflow");
                });
                return false;
            }
            asio::post(strand_, [self] { self->drain(); });
            return true;
        }

    private:
        struct QueuedOutput {
            EndpointOutput output;
            std::size_t bytes = 0;
        };

        static constexpr std::size_t static_chunk_bytes = 64 * 1024;

        struct StaticTransfer {
            using Response = http::response<http::buffer_body>;
            using Serializer = http::response_serializer<http::buffer_body>;

            std::filesystem::path path;
            std::uintmax_t offset = 0;
            std::uintmax_t remaining = 0;
            std::array<char, static_chunk_bytes> buffer{};
            std::shared_ptr<Response> response;
            std::shared_ptr<Serializer> serializer;
        };

        void open(std::shared_ptr<EndpointSink> sink) {
            if (started_ || finished_) return;
            started_ = true;
            try {
                EndpointRequest endpoint_request;
                endpoint_request.protocol = websocket::is_upgrade(request_)
                    ? EndpointProtocol::websocket
                    : EndpointProtocol::http;
                endpoint_request.method.assign(request_.method_string().data(),
                    request_.method_string().size());

                const std::string_view target(
                    request_.target().data(), request_.target().size());
                const auto fragment = target.find('#');
                const auto question = target.find('?');
                const auto path_end = std::min(
                    question == std::string_view::npos ? target.size() : question,
                    fragment == std::string_view::npos ? target.size() : fragment);
                if (target.empty() || target.front() != '/' ||
                    fragment != std::string_view::npos)
                    return write_gateway_error(http::status::bad_request,
                        "request target must be an absolute origin path");
                endpoint_request.path.assign(target.substr(0, path_end));
                if (question != std::string_view::npos)
                    endpoint_request.query.assign(target.substr(question + 1));
                endpoint_request.headers.reserve(
                    static_cast<std::size_t>(std::distance(
                        request_.base().begin(), request_.base().end())));
                for (const auto& field : request_) {
                    if (field.name() == http::field::cookie) {
                        const auto cookie = strip_core_session_cookie(
                            std::string_view(field.value().data(),
                                field.value().size()));
                        if (cookie.value)
                            endpoint_request.headers.emplace_back(
                                "Cookie", std::move(*cookie.value));
                        continue;
                    }
                    if (beast::iequals(field.name_string(),
                            "X-Circud-CSRF"))
                        continue;
                    endpoint_request.headers.emplace_back(
                        std::string(field.name_string()),
                        std::string(field.value()));
                }
                endpoint_request.body = copy_bytes(request_.body());
                endpoint_request.is_authorized = is_authorized_;
                if (endpoint_request.protocol == EndpointProtocol::websocket) {
                    const auto protocol_header =
                        request_[http::field::sec_websocket_protocol];
                    endpoint_request.offered_subprotocols =
                        offered_subprotocols(protocol_header);
                    if (!protocol_header.empty() &&
                        endpoint_request.offered_subprotocols.empty())
                        return write_gateway_error(http::status::bad_request,
                            "invalid WebSocket subprotocol list");
                }
                boost::system::error_code remote_error;
                const auto remote = stream_->socket().remote_endpoint(remote_error);
                if (!remote_error)
                    endpoint_request.remote_address =
                        remote.address().to_string() + ":" +
                        std::to_string(remote.port());
                endpoint_request.deadline = std::chrono::steady_clock::now() +
                    core_.config().endpoints.idle_timeout;

                const auto result = core_.endpoint_gateway().open(
                    std::move(endpoint_request), std::move(sink));
                if (!result)
                    return write_gateway_error(gateway_status(result.status),
                        result.message.empty() ?
                            "package endpoint is unavailable" : result.message);
                session_id_ = result.session_id;
                package_id_ = result.package_id;
                gateway_active_ = true;
                static_root_ = std::move(result.package_version_dir);
                stream_->expires_never();
                arm_idle_timer();
                arm_lifetime_timer();
                drain();
            }
            catch (...) {
                finish(true, 1011, "cannot open endpoint session");
            }
        }

        void arm_idle_timer() {
            if (finished_) return;
            idle_timer_.expires_after(core_.config().endpoints.idle_timeout);
            auto self = shared_from_this();
            idle_timer_.async_wait(asio::bind_executor(strand_,
                [self](boost::system::error_code error) {
                    if (!error)
                        self->finish(true, 1001, "endpoint idle timeout");
                }));
        }

        void arm_lifetime_timer() {
            lifetime_timer_.expires_after(
                core_.config().endpoints.max_lifetime);
            auto self = shared_from_this();
            lifetime_timer_.async_wait(asio::bind_executor(strand_,
                [self](boost::system::error_code error) {
                    if (!error)
                        self->finish(true, 1001,
                            "endpoint maximum lifetime reached");
                }));
        }

        void drain() {
            if (finished_ || writing_ || current_) return;
            {
                std::lock_guard lock(queue_mutex_);
                if (queue_.empty()) return;
                current_.emplace(std::move(queue_.front()));
                queue_.pop_front();
            }
            try {
                std::visit([this](auto& output) { process(output); },
                    current_->output);
            }
            catch (...) {
                finish(true, 1011, "invalid endpoint output");
            }
        }

        void process(EndpointResponse& response) {
            gateway_active_ = false;
            if (response.status < 200 || response.status > 599 ||
                response.body.size() >
                    core_.config().endpoints.max_response_body_bytes ||
                (request_.method() == http::verb::connect &&
                    response.status < 300) ||
                (!response.body.empty() && response_status_forbids_body(
                    response.status, request_.method())) ||
                !validate_headers(response.headers))
                return fail_current(http::status::bad_gateway,
                    "package returned an invalid HTTP response");
            write_response(response.status, response.headers, response.body);
        }

        void process(EndpointStatic& instruction) {
            gateway_active_ = false;
            if (request_.method() == http::verb::connect)
                return fail_current(http::status::method_not_allowed,
                    "CONNECT cannot serve a static response");
            serve_static(instruction.relative_path);
        }

        void process(EndpointAccept& accept) {
            if (accepted_protocol_ != std::nullopt ||
                !validate_headers(accept.headers,
                    accept.protocol == EndpointProtocol::websocket))
                return finish(true, 1011, "invalid endpoint accept output");
            if (accept.protocol == EndpointProtocol::sse)
                accept_sse(accept);
            else if (accept.protocol == EndpointProtocol::websocket)
                accept_websocket(accept);
            else
                finish(true, 1011, "invalid endpoint protocol acceptance");
        }

        void process(EndpointMessage& message) {
            if (!accepted_protocol_ ||
                message.data.size() > core_.config().endpoints.max_message_bytes)
                return finish(true, 1011, "invalid endpoint message");
            if (*accepted_protocol_ == EndpointProtocol::sse) {
                if (message.binary || message.data.empty() ||
                    !valid_utf8(message.data))
                    return finish(true, 1011, "SSE output must be UTF-8 text");
                write_sse(message.data);
            }
            else if (*accepted_protocol_ == EndpointProtocol::websocket) {
                write_websocket(message);
            }
            else {
                finish(true, 1011, "invalid endpoint message protocol");
            }
        }

        void process(EndpointClose& close) {
            gateway_active_ = false;
            if (accepted_protocol_ == EndpointProtocol::websocket && websocket_)
                close_websocket(close);
            else if (accepted_protocol_ == EndpointProtocol::sse)
                close_sse();
            else
                finish(false, close.code, close.reason);
        }

        void write_gateway_error(http::status status, std::string message) {
            auto response = std::make_shared<StringResponse>(status,
                request_.version());
            response->set(http::field::server, "circud/0.1");
            response->set(http::field::content_type,
                "text/plain; charset=utf-8");
            response->keep_alive(false);
            response->body() = std::move(message);
            if (request_.method() == http::verb::head) {
                response->content_length(response->body().size());
                response->body().clear();
            }
            else {
                response->prepare_payload();
            }
            writing_ = true;
            stream_->expires_after(core_.config().http_io_timeout);
            auto self = shared_from_this();
            http::async_write(*stream_, *response,
                asio::bind_executor(strand_,
                    [self, response](boost::system::error_code, std::size_t) {
                        self->writing_ = false;
                        self->finish(false, 1000, {});
                    }));
        }

        void fail_current(http::status status, std::string_view message) {
            EndpointHeaders headers{
                { "content-type", "text/plain; charset=utf-8" } };
            write_response(static_cast<unsigned>(status), headers,
                copy_bytes(message));
        }

        void write_response(unsigned status, const EndpointHeaders& headers,
            const EndpointBytes& body) {
            auto response = std::make_shared<StringResponse>(
                static_cast<http::status>(status), request_.version());
            response->set(http::field::server, "circud/0.1");
            response->set("X-Content-Type-Options", "nosniff");
            for (const auto& [name, value] : headers)
                response->insert(name, value);
            response->keep_alive(can_handoff());
            const bool body_forbidden = response_status_forbids_body(
                status, request_.method());
            if (!body_forbidden)
                response->body().assign(
                    reinterpret_cast<const char*>(body.data()), body.size());
            if (request_.method() == http::verb::head && !body_forbidden) {
                response->content_length(body.size());
                response->body().clear();
            }
            else if (status == 205) {
                // Unlike 1xx, 204 and 304, HTTP/1.1 framing does not infer an
                // empty message body for 205. State the zero length explicitly
                // before handing a persistent connection back to the listener.
                response->content_length(0);
            }
            else if (!body_forbidden) {
                response->prepare_payload();
            }
            writing_ = true;
            stream_->expires_after(core_.config().http_io_timeout);
            auto self = shared_from_this();
            http::async_write(*stream_, *response,
                asio::bind_executor(strand_,
                    [self, response](boost::system::error_code error,
                        std::size_t) {
                        self->writing_ = false;
                        self->release_current();
                        self->finish_http_response(error);
                    }));
        }

        void serve_static(const std::string& relative_text) {
            const std::filesystem::path relative(relative_text);
            if (static_root_.empty() || !safe_relative_path(relative))
                return fail_current(http::status::not_found,
                    "static file not found");
            const auto size_limit = std::min(
                core_.config().http_max_static_file_bytes,
                core_.config().endpoints.max_response_body_bytes);
            writing_ = true;
            auto self = shared_from_this();
            const auto root = static_root_;
            const auto method = request_.method();
            const auto version = request_.version();
            const auto keep_alive = can_handoff();
            const bool accepted = background_->post(0,
                [self, root, relative, size_limit, method, version,
                    keep_alive] {
                    std::error_code error;
                    const auto canonical_root =
                        std::filesystem::canonical(root, error);
                    std::filesystem::path candidate;
                    if (!error)
                        candidate = std::filesystem::canonical(
                            canonical_root / relative, error);
                    if (error || !path_within(candidate, canonical_root) ||
                        !std::filesystem::is_regular_file(candidate, error)) {
                        return asio::post(self->strand_, [self] {
                            self->writing_ = false;
                            if (!self->finished_)
                                self->fail_current(http::status::not_found,
                                    "static file not found");
                        });
                    }
                    const auto size =
                        std::filesystem::file_size(candidate, error);
                    if (error || size > size_limit) {
                        return asio::post(self->strand_, [self] {
                            self->writing_ = false;
                            if (!self->finished_)
                                self->fail_current(
                                    http::status::payload_too_large,
                                    "static file exceeds configured limit");
                        });
                    }
                    if (method == http::verb::head) {
                        auto response = std::make_shared<StringResponse>(
                            http::status::ok, version);
                        response->set(http::field::server, "circud/0.1");
                        response->set(http::field::content_type,
                            content_type(candidate));
                        response->content_length(size);
                        response->keep_alive(keep_alive);
                        return asio::post(self->strand_,
                            [self, response = std::move(response)] {
                                self->write_static_response(
                                    std::move(response));
                            });
                    }
                    auto transfer = std::make_shared<StaticTransfer>();
                    const auto file = ::open(candidate.c_str(),
                        O_RDONLY | O_CLOEXEC);
                    if (file < 0) {
                        return asio::post(self->strand_, [self] {
                            self->writing_ = false;
                            if (!self->finished_)
                                self->fail_current(http::status::not_found,
                                    "static file not found");
                        });
                    }
                    ::close(file);
                    transfer->path = candidate;
                    transfer->remaining = size;
                    transfer->response = std::make_shared<
                        StaticTransfer::Response>(http::status::ok, version);
                    transfer->response->set(
                        http::field::server, "circud/0.1");
                    transfer->response->set(http::field::content_type,
                        content_type(candidate));
                    transfer->response->content_length(size);
                    transfer->response->keep_alive(keep_alive);
                    asio::post(self->strand_,
                        [self, transfer = std::move(transfer)] {
                            self->read_static_chunk(std::move(transfer));
                        });
                }, [self] {
                    asio::post(self->strand_, [self] {
                        if (!self->finished_)
                            self->finish(true, 1001,
                                "HTTP filesystem generation stopped");
                    });
                });
            if (!accepted) {
                writing_ = false;
                fail_current(http::status::service_unavailable,
                    "HTTP filesystem queue is unavailable");
            }
        }

        void read_static_chunk(std::shared_ptr<StaticTransfer> transfer) {
            if (finished_) return;
            const bool accepted = background_->post(static_chunk_bytes,
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
                        return asio::post(self->strand_, [self, transfer] {
                            if (self->finished_) return;
                            self->writing_ = false;
                            if (transfer->serializer)
                                return self->finish(false, 1000, {});
                            self->fail_current(http::status::not_found,
                                "static file could not be read");
                        });
                    }
                    const auto bytes = static_cast<std::size_t>(amount);
                    transfer->offset += bytes;
                    transfer->remaining -= bytes;
                    const bool final = transfer->remaining == 0;
                    asio::post(self->strand_,
                        [self, transfer, bytes, final] {
                            self->write_static_chunk(
                                std::move(transfer), bytes, final);
                        });
                });
            if (!accepted) {
                writing_ = false;
                fail_current(http::status::service_unavailable,
                    "HTTP filesystem queue is unavailable");
            }
        }

        void write_static_chunk(std::shared_ptr<StaticTransfer> transfer,
            std::size_t bytes, bool final) {
            if (finished_) return;
            transfer->response->body().data = transfer->buffer.data();
            transfer->response->body().size = bytes;
            transfer->response->body().more = !final;
            if (!transfer->serializer)
                transfer->serializer = std::make_shared<
                    StaticTransfer::Serializer>(*transfer->response);
            stream_->expires_after(core_.config().http_io_timeout);
            http::async_write(*stream_, *transfer->serializer,
                asio::bind_executor(strand_,
                    [self = shared_from_this(), transfer, final](
                        boost::system::error_code error, std::size_t) {
                        if (error == http::error::need_buffer && !final)
                            return self->read_static_chunk(transfer);
                        self->writing_ = false;
                        self->release_current();
                        self->finish_http_response(error);
                    }));
        }

        template <typename Body>
        void write_static_response(
            std::shared_ptr<http::response<Body>> response) {
            if (finished_) return;
            stream_->expires_after(core_.config().http_io_timeout);
            http::async_write(*stream_, *response,
                asio::bind_executor(strand_,
                    [self = shared_from_this(), response](
                        boost::system::error_code error, std::size_t) {
                        self->writing_ = false;
                        self->release_current();
                        self->finish_http_response(error);
                    }));
        }

        bool can_handoff() const noexcept {
            // circud intentionally closes HTTP/1.0 package requests even when
            // a client asks for its optional keep-alive extension. This keeps
            // the handoff contract strictly HTTP/1.1.
            return request_.version() == 11 && request_.keep_alive();
        }

        void finish_http_response(boost::system::error_code error) {
            if (error || !can_handoff() || !stream_)
                return finish(false, 1000, {});

            Handoff handoff{
                std::move(*stream_), std::move(buffer_) };
            stream_.reset();
            finish(false, 1000, {}, std::move(handoff));
        }

        void accept_sse(const EndpointAccept& accept) {
            if (request_.method() != http::verb::get ||
                request_.version() < 11 || accept.status != 200)
                return finish(true, 1011,
                    "SSE requires a GET request and HTTP/1.1 status 200");
            auto response = std::make_shared<http::response<http::empty_body>>(
                static_cast<http::status>(accept.status), request_.version());
            response->set(http::field::server, "circud/0.1");
            response->set("X-Accel-Buffering", "no");
            bool has_content_type = false;
            for (const auto& [name, value] : accept.headers) {
                response->insert(name, value);
                if (beast::iequals(name, "content-type")) {
                    has_content_type = media_type_is(
                        value, "text/event-stream");
                }
            }
            if (!has_content_type)
                return finish(true, 1011,
                    "SSE response requires text/event-stream");
            response->keep_alive(true);
            response->chunked(true);
            auto serializer = std::make_shared<
                http::response_serializer<http::empty_body>>(*response);
            writing_ = true;
            auto self = shared_from_this();
            http::async_write_header(*stream_, *serializer,
                asio::bind_executor(strand_,
                    [self, response, serializer](boost::system::error_code error,
                        std::size_t) {
                        self->writing_ = false;
                        if (error)
                            return self->finish(true, 1001,
                                "SSE client disconnected");
                        self->stream_->expires_never();
                        self->accepted_protocol_ = EndpointProtocol::sse;
                        self->arm_idle_timer();
                        self->release_current();
                        self->drain();
                    }));
        }

        void write_sse(const EndpointBytes& data) {
            writing_ = true;
            auto self = shared_from_this();
            // finish() may clear current_ while an async write is being
            // cancelled by an idle/lifetime timeout or daemon shutdown. Keep
            // the bytes independently alive until the write handler runs.
            auto payload = std::make_shared<EndpointBytes>(data);
            const auto body = asio::buffer(payload->data(), payload->size());
            auto chunk = std::make_shared<decltype(http::make_chunk(body))>(
                http::make_chunk(body));
            asio::async_write(*stream_, *chunk,
                asio::bind_executor(strand_,
                    [self, chunk, payload](boost::system::error_code error,
                        std::size_t) {
                        self->writing_ = false;
                        if (error)
                            return self->finish(true, 1001,
                                "SSE client disconnected");
                        self->arm_idle_timer();
                        self->release_current();
                        self->drain();
                    }));
        }

        void close_sse() {
            writing_ = true;
            auto self = shared_from_this();
            auto chunk = std::make_shared<decltype(http::make_chunk_last())>(
                http::make_chunk_last());
            asio::async_write(*stream_, *chunk,
                asio::bind_executor(strand_,
                    [self, chunk](boost::system::error_code, std::size_t) {
                        self->writing_ = false;
                        self->release_current();
                        self->finish(false, 1000, {});
                    }));
        }

        void accept_websocket(const EndpointAccept& accept) {
            if (!websocket::is_upgrade(request_) || accept.status != 101)
                return finish(true, 1002, "invalid WebSocket acceptance");
            if (!accept.subprotocol.empty()) {
                const auto offered = offered_subprotocols(
                    request_[http::field::sec_websocket_protocol]);
                if (std::ranges::find(offered, accept.subprotocol) ==
                    offered.end())
                    return finish(true, 1002,
                        "WebSocket subprotocol was not offered");
            }

            websocket_.emplace(std::move(*stream_));
            stream_.reset();
            websocket_->read_message_max(
                core_.config().endpoints.max_message_bytes);
            websocket_->set_option(websocket::stream_base::timeout::suggested(
                beast::role_type::server));
            const auto headers = accept.headers;
            const auto subprotocol = accept.subprotocol;
            websocket_->set_option(websocket::stream_base::decorator(
                [headers, subprotocol](
                    websocket::response_type& response) {
                    for (const auto& [name, value] : headers)
                        response.insert(name, value);
                    if (!subprotocol.empty())
                        response.set(http::field::sec_websocket_protocol,
                            subprotocol);
                    response.set(http::field::server, "circud/0.1");
                }));
            writing_ = true;
            auto self = shared_from_this();
            auto accepted = [self](boost::system::error_code error) {
                    self->writing_ = false;
                    if (error)
                        return self->finish(true, 1002,
                            "WebSocket upgrade failed");
                    self->accepted_protocol_ = EndpointProtocol::websocket;
                    self->arm_idle_timer();
                    self->release_current();
                    self->read_websocket();
                    self->drain();
                };
            if (buffer_.size() == 0)
                websocket_->async_accept(request_, asio::bind_executor(
                    strand_, std::move(accepted)));
            else {
                std::string handshake;
                handshake.reserve(256 + buffer_.size());
                handshake.append(request_.method_string());
                handshake.push_back(' ');
                handshake.append(request_.target());
                handshake.append(" HTTP/");
                handshake.append(std::to_string(request_.version() / 10));
                handshake.push_back('.');
                handshake.append(std::to_string(request_.version() % 10));
                handshake.append("\r\n");
                for (const auto& field : request_) {
                    handshake.append(field.name_string());
                    handshake.append(": ");
                    handshake.append(field.value());
                    handshake.append("\r\n");
                }
                handshake.append("\r\n");
                handshake.append(beast::buffers_to_string(buffer_.data()));
                websocket_->async_accept(asio::buffer(handshake),
                    asio::bind_executor(strand_, std::move(accepted)));
                // async_accept copies caller-provided buffers before it
                // returns; keep the dynamic read buffer empty for frames.
                buffer_.consume(buffer_.size());
            }
        }

        void read_websocket() {
            if (finished_ || !websocket_ || reading_) return;
            reading_ = true;
            auto self = shared_from_this();
            websocket_->async_read(buffer_, asio::bind_executor(strand_,
                [self](boost::system::error_code error, std::size_t bytes) {
                    self->reading_ = false;
                    if (error == websocket::error::closed) {
                        const auto reason = self->websocket_->reason();
                        const auto code = reason.code ==
                                static_cast<std::uint16_t>(
                                    websocket::close_code::none)
                            ? static_cast<std::uint16_t>(
                                websocket::close_code::no_status)
                            : reason.code;
                        return self->finish(true,
                            code,
                            std::string(reason.reason));
                    }
                    if (error)
                        return self->finish(true, 1001,
                            "WebSocket client disconnected");
                    if (bytes >
                        self->core_.config().endpoints.max_message_bytes)
                        return self->finish(true, 1009,
                            "WebSocket message exceeds configured limit");
                    EndpointBytes data(bytes);
                    asio::buffer_copy(asio::buffer(data.data(), data.size()),
                        self->buffer_.data());
                    self->buffer_.consume(bytes);
                    const bool binary = self->websocket_->got_binary();
                    if (!self->core_.endpoint_gateway().message(
                            self->session_id_, std::move(data), binary))
                        return self->finish(false, 1013,
                            "package endpoint queue is unavailable");
                    self->arm_idle_timer();
                    self->read_websocket();
                }));
        }

        void write_websocket(const EndpointMessage& message) {
            websocket_->binary(message.binary);
            writing_ = true;
            auto self = shared_from_this();
            // The session can finish and release current_ before cancellation
            // reaches this handler. Own the in-flight frame independently.
            auto payload = std::make_shared<EndpointBytes>(message.data);
            websocket_->async_write(asio::buffer(
                    payload->data(), payload->size()),
                asio::bind_executor(strand_,
                    [self, payload](boost::system::error_code error,
                        std::size_t) {
                        self->writing_ = false;
                        if (error)
                            return self->finish(true, 1001,
                                "WebSocket client disconnected");
                        self->arm_idle_timer();
                        self->release_current();
                        self->drain();
                    }));
        }

        void close_websocket(const EndpointClose& close) {
            websocket::close_reason reason;
            reason.code = static_cast<websocket::close_code>(
                valid_websocket_close_code(close.code) ? close.code : 1011);
            reason.reason = close.reason;
            writing_ = true;
            auto self = shared_from_this();
            websocket_->async_close(reason, asio::bind_executor(strand_,
                [self](boost::system::error_code) {
                    self->writing_ = false;
                    self->release_current();
                    self->finish(false, 1000, {});
                }));
        }

        void release_current() {
            if (!current_) return;
            std::lock_guard lock(queue_mutex_);
            if (queued_messages_ != 0) --queued_messages_;
            queued_bytes_ = current_->bytes > queued_bytes_
                ? 0 : queued_bytes_ - current_->bytes;
            current_.reset();
        }

        void finish(bool notify_gateway, std::uint16_t code,
            std::string reason,
            std::optional<Handoff> handoff = std::nullopt) {
            if (finished_) return;
            finished_ = true;
            idle_timer_.cancel();
            lifetime_timer_.cancel();
            {
                std::lock_guard lock(queue_mutex_);
                accepting_outputs_ = false;
                queue_.clear();
                queued_messages_ = 0;
                queued_bytes_ = 0;
                current_.reset();
            }
            if (!handoff) close_socket();
            if (notify_gateway && gateway_active_ && session_id_ != 0) {
                gateway_active_ = false;
                (void)core_.endpoint_gateway().close(
                    session_id_, code, std::move(reason));
            }
            if (completion_) {
                auto completion = std::move(completion_);
                try {
                    completion(std::move(handoff));
                }
                catch (...) {
                    // A completion callback cannot escape the I/O loop.
                }
            }
        }

        void close_socket() noexcept {
            boost::system::error_code ignored;
            if (websocket_) {
                auto& socket = beast::get_lowest_layer(*websocket_).socket();
                socket.cancel(ignored);
                socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                socket.close(ignored);
            }
            else if (stream_) {
                stream_->socket().cancel(ignored);
                stream_->socket().shutdown(
                    asio::ip::tcp::socket::shutdown_both, ignored);
                stream_->socket().close(ignored);
            }
        }

        Core& core_;
        asio::strand<asio::io_context::executor_type> strand_;
        asio::steady_timer idle_timer_;
        asio::steady_timer lifetime_timer_;
        std::shared_ptr<HttpBackground> background_;
        std::optional<beast::tcp_stream> stream_;
        std::optional<websocket::stream<beast::tcp_stream>> websocket_;
        beast::flat_buffer buffer_;
        StringRequest request_;
        bool is_authorized_ = false;
        Completion completion_;

        std::mutex queue_mutex_;
        std::deque<QueuedOutput> queue_;
        std::optional<QueuedOutput> current_;
        std::size_t queued_messages_ = 0;
        std::size_t queued_bytes_ = 0;
        bool accepting_outputs_ = true;

        std::filesystem::path static_root_;
        std::string package_id_;
        std::uint64_t session_id_ = 0;
        std::optional<EndpointProtocol> accepted_protocol_;
        bool gateway_active_ = false;
        bool started_ = false;
        bool writing_ = false;
        bool reading_ = false;
        bool finished_ = false;
    };

    EndpointSession::EndpointSession(Core& core,
        std::shared_ptr<HttpBackground> background,
        beast::tcp_stream&& stream,
        beast::flat_buffer&& buffer, StringRequest&& request,
        bool is_authorized,
        Completion completion)
        : state_(std::make_shared<State>(core,
              std::move(background), std::move(stream),
              std::move(buffer), std::move(request),
              is_authorized,
              std::move(completion))) {}

    EndpointSession::~EndpointSession() = default;

    void EndpointSession::run() {
        state_->run(std::static_pointer_cast<EndpointSink>(shared_from_this()));
    }

    void EndpointSession::close() {
        state_->request_close();
    }

    bool EndpointSession::emit(EndpointOutput output) {
        return state_->emit(std::move(output));
    }

} // namespace circud
