#include <circud/config.hpp>
#include <circud/auth.hpp>

#include "json.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/json/object.hpp>

#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace circud {
    namespace {

        constexpr std::size_t kMaxConfigBytes = 256 * 1024;

        [[noreturn]] void config_error(std::string_view field,
            std::string_view message) {
            throw std::runtime_error("config " + std::string(field) + ": " +
                std::string(message));
        }

        void reject_unknown(const boost::json::object& object,
            std::initializer_list<std::string_view> allowed,
            std::string_view scope) {
            for (const auto& item : object) {
                bool found = false;
                for (const auto name : allowed) {
                    if (item.key() == name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    const auto prefix = scope.empty() ? std::string{}
                    : std::string(scope) + ".";
                    throw std::runtime_error("unknown config field: " + prefix +
                        std::string(item.key()));
                }
            }
        }

        const boost::json::object& require_object(const boost::json::value& value,
            std::string_view field) {
            if (!value.is_object()) config_error(field, "must be an object");
            return value.as_object();
        }

        std::string require_string(const boost::json::value& value,
            std::string_view field) {
            if (!value.is_string()) config_error(field, "must be a string");
            return std::string(value.as_string());
        }

        bool require_bool(const boost::json::value& value, std::string_view field) {
            if (!value.is_bool()) config_error(field, "must be boolean");
            return value.as_bool();
        }

        std::uint64_t require_unsigned(const boost::json::value& value,
            std::string_view field) {
            if (value.is_uint64()) return value.as_uint64();
            if (value.is_int64() && value.as_int64() >= 0)
                return static_cast<std::uint64_t>(value.as_int64());
            config_error(field, "must be a non-negative integer");
        }

        std::uint64_t parse_unsigned(std::string_view text, std::string_view option) {
            std::uint64_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw std::runtime_error(std::string(option) +
                    " must be a non-negative integer");
            return value;
        }

        template <typename Target>
        Target checked_number(std::uint64_t value, std::uint64_t minimum,
            std::uint64_t maximum, std::string_view field) {
            if (value < minimum || value > maximum) {
                throw std::runtime_error(std::string(field) + " must be " +
                    std::to_string(minimum) + ".." +
                    std::to_string(maximum));
            }
            return static_cast<Target>(value);
        }

        std::chrono::milliseconds checked_milliseconds(std::uint64_t value,
            std::uint64_t minimum,
            std::uint64_t maximum,
            std::string_view field) {
            return std::chrono::milliseconds(
                checked_number<std::int64_t>(value, minimum, maximum, field));
        }

        std::pair<std::string, std::uint16_t> parse_listen(std::string_view value) {
            std::string_view address;
            std::string_view port_text;
            if (value.starts_with('[')) {
                const auto close = value.find(']');
                if (close == std::string_view::npos || close + 1 >= value.size() ||
                    value[close + 1] != ':')
                    throw std::runtime_error(
                        "listen must be ADDRESS:PORT or [IPv6]:PORT");
                address = value.substr(1, close - 1);
                port_text = value.substr(close + 2);
            }
            else {
                const auto separator = value.rfind(':');
                if (separator == std::string_view::npos)
                    throw std::runtime_error(
                        "listen must be ADDRESS:PORT or [IPv6]:PORT");
                address = value.substr(0, separator);
                port_text = value.substr(separator + 1);
            }
            if (address.empty()) throw std::runtime_error("listen address is empty");
            const auto port = checked_number<std::uint16_t>(
                parse_unsigned(port_text, "listen port"), 1, 65535, "listen port");
            return { std::string(address), port };
        }

        std::filesystem::path config_path(const boost::json::value& value,
            std::string_view field,
            const std::filesystem::path& base) {
            std::filesystem::path path(require_string(value, field));
            if (path.empty()) config_error(field, "must not be empty");
            if (path.is_relative()) path = base / path;
            return path.lexically_normal();
        }

        void apply_config_file(const std::filesystem::path& path, CoreConfig& config) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                throw std::runtime_error("cannot open config file: " + path.string());
            stream.seekg(0, std::ios::end);
            const auto size = stream.tellg();
            if (size < 0 || static_cast<std::uint64_t>(size) > kMaxConfigBytes)
                throw std::runtime_error("config file exceeds 256 KiB");
            stream.seekg(0, std::ios::beg);
            std::string text(static_cast<std::size_t>(size), '\0');
            if (!text.empty() && !stream.read(text.data(), size))
                throw std::runtime_error("cannot read config file: " + path.string());
            const auto document = parse_json(text);
            const auto& root = require_object(document, "root");
            reject_unknown(root,
                { "stateDir", "listen", "web", "status", "calls", "events",
                  "auth", "endpoints", "runtimeStartTimeoutMs" },
                "");
            const auto base = std::filesystem::absolute(path).parent_path();

            if (const auto* value = root.if_contains("stateDir"))
                config.state_dir = config_path(*value, "stateDir", base);
            if (const auto* value = root.if_contains("runtimeStartTimeoutMs"))
                config.runtime_start_timeout = checked_milliseconds(
                    require_unsigned(*value, "runtimeStartTimeoutMs"), 100,
                    300000, "runtimeStartTimeoutMs");
            if (const auto* value = root.if_contains("listen")) {
                const auto& listen = require_object(*value, "listen");
                reject_unknown(listen, { "address", "port" }, "listen");
                if (const auto* address = listen.if_contains("address")) {
                    config.listen_address = require_string(*address, "listen.address");
                    if (config.listen_address.empty())
                        config_error("listen.address", "must not be empty");
                }
                if (const auto* port = listen.if_contains("port"))
                    config.listen_port = checked_number<std::uint16_t>(
                        require_unsigned(*port, "listen.port"), 1, 65535,
                        "listen.port");
            }
            if (const auto* value = root.if_contains("web")) {
                const auto& web = require_object(*value, "web");
                reject_unknown(web,
                    { "enabled", "publicDir", "backgroundWorkers",
                      "maxConnections", "ioTimeoutMs", "maxBodyBytes",
                      "maxUploadBytes", "maxStaticFileBytes" },
                    "web");
                if (const auto* item = web.if_contains("enabled"))
                    config.web_enabled = require_bool(*item, "web.enabled");
                if (const auto* item = web.if_contains("publicDir"))
                    config.public_dir = config_path(*item, "web.publicDir", base);
                if (const auto* item = web.if_contains("backgroundWorkers"))
                    config.http_background_workers = checked_number<std::size_t>(
                        require_unsigned(*item, "web.backgroundWorkers"), 1, 16,
                        "web.backgroundWorkers");
                if (const auto* item = web.if_contains("maxConnections"))
                    config.http_max_connections = checked_number<std::size_t>(
                        require_unsigned(*item, "web.maxConnections"), 1, 4096,
                        "web.maxConnections");
                if (const auto* item = web.if_contains("ioTimeoutMs"))
                    config.http_io_timeout = checked_milliseconds(
                        require_unsigned(*item, "web.ioTimeoutMs"), 100, 300000,
                        "web.ioTimeoutMs");
                if (const auto* item = web.if_contains("maxBodyBytes"))
                    config.http_max_body_bytes = checked_number<std::size_t>(
                        require_unsigned(*item, "web.maxBodyBytes"), 1024, 64 * 1024,
                        "web.maxBodyBytes");
                if (const auto* item = web.if_contains("maxUploadBytes"))
                    config.http_max_upload_bytes = checked_number<std::size_t>(
                        require_unsigned(*item, "web.maxUploadBytes"), 1024,
                        64 * 1024 * 1024, "web.maxUploadBytes");
                if (const auto* item = web.if_contains("maxStaticFileBytes"))
                    config.http_max_static_file_bytes = checked_number<std::size_t>(
                        require_unsigned(*item, "web.maxStaticFileBytes"), 1024,
                        64 * 1024 * 1024, "web.maxStaticFileBytes");
            }
            if (const auto* value = root.if_contains("status")) {
                const auto& status = require_object(*value, "status");
                reject_unknown(status, { "enabled", "intervalMs", "timeoutMs" },
                    "status");
                if (const auto* item = status.if_contains("enabled"))
                    config.status_polling_enabled = require_bool(*item, "status.enabled");
                if (const auto* item = status.if_contains("intervalMs"))
                    config.status_interval = checked_milliseconds(
                        require_unsigned(*item, "status.intervalMs"), 1000, 3600000,
                        "status.intervalMs");
                if (const auto* item = status.if_contains("timeoutMs"))
                    config.status_timeout = checked_milliseconds(
                        require_unsigned(*item, "status.timeoutMs"), 100, 300000,
                        "status.timeoutMs");
            }
            if (const auto* value = root.if_contains("calls")) {
                const auto& calls = require_object(*value, "calls");
                reject_unknown(calls, { "defaultTimeoutMs" }, "calls");
                if (const auto* item = calls.if_contains("defaultTimeoutMs"))
                    config.default_call_timeout = checked_milliseconds(
                        require_unsigned(*item, "calls.defaultTimeoutMs"), 1, 300000,
                        "calls.defaultTimeoutMs");
            }
            if (const auto* value = root.if_contains("events")) {
                const auto& events = require_object(*value, "events");
                reject_unknown(events, { "enabled", "capacity" }, "events");
                if (const auto* item = events.if_contains("enabled"))
                    config.events_enabled = require_bool(*item, "events.enabled");
                if (const auto* item = events.if_contains("capacity"))
                    config.event_capacity = checked_number<std::size_t>(
                        require_unsigned(*item, "events.capacity"), 32, 4096,
                        "events.capacity");
            }
            if (const auto* value = root.if_contains("auth")) {
                const auto& auth = require_object(*value, "auth");
                reject_unknown(auth,
                    { "enabled", "username", "passwordHash",
                      "sessionIdleTimeoutMs", "sessionMaxLifetimeMs",
                      "maxSessions", "secureCookie" },
                    "auth");
                if (const auto* item = auth.if_contains("enabled"))
                    config.auth.enabled = require_bool(*item, "auth.enabled");
                if (const auto* item = auth.if_contains("username"))
                    config.auth.username = require_string(*item, "auth.username");
                if (const auto* item = auth.if_contains("passwordHash"))
                    config.auth.password_hash =
                        require_string(*item, "auth.passwordHash");
                if (const auto* item =
                        auth.if_contains("sessionIdleTimeoutMs"))
                    config.auth.session_idle_timeout = checked_milliseconds(
                        require_unsigned(*item, "auth.sessionIdleTimeoutMs"),
                        1000, 86400000, "auth.sessionIdleTimeoutMs");
                if (const auto* item =
                        auth.if_contains("sessionMaxLifetimeMs"))
                    config.auth.session_max_lifetime = checked_milliseconds(
                        require_unsigned(*item, "auth.sessionMaxLifetimeMs"),
                        1000, 2592000000ULL,
                        "auth.sessionMaxLifetimeMs");
                if (const auto* item = auth.if_contains("maxSessions"))
                    config.auth.max_sessions = checked_number<std::size_t>(
                        require_unsigned(*item, "auth.maxSessions"), 1, 4096,
                        "auth.maxSessions");
                if (const auto* item = auth.if_contains("secureCookie"))
                    config.auth.secure_cookie =
                        require_bool(*item, "auth.secureCookie");
            }
            if (const auto* value = root.if_contains("endpoints")) {
                const auto& endpoints = require_object(*value, "endpoints");
                reject_unknown(endpoints,
                    { "enabled", "maxSessions", "maxSessionsPerPackage",
                      "maxRequestBodyBytes", "maxResponseBodyBytes",
                      "maxMessageBytes", "maxQueuedMessages", "maxQueuedBytes",
                      "idleTimeoutMs", "maxLifetimeMs" },
                    "endpoints");
                if (const auto* item = endpoints.if_contains("enabled"))
                    config.endpoints.enabled =
                        require_bool(*item, "endpoints.enabled");
                if (const auto* item = endpoints.if_contains("maxSessions"))
                    config.endpoints.max_sessions = checked_number<std::size_t>(
                        require_unsigned(*item, "endpoints.maxSessions"), 1, 4096,
                        "endpoints.maxSessions");
                if (const auto* item =
                        endpoints.if_contains("maxSessionsPerPackage"))
                    config.endpoints.max_sessions_per_package =
                        checked_number<std::size_t>(
                            require_unsigned(
                                *item, "endpoints.maxSessionsPerPackage"),
                            1, 1024, "endpoints.maxSessionsPerPackage");
                if (const auto* item =
                        endpoints.if_contains("maxRequestBodyBytes"))
                    config.endpoints.max_request_body_bytes =
                        checked_number<std::size_t>(
                            require_unsigned(
                                *item, "endpoints.maxRequestBodyBytes"),
                            1024, 16 * 1024 * 1024,
                            "endpoints.maxRequestBodyBytes");
                if (const auto* item =
                        endpoints.if_contains("maxResponseBodyBytes"))
                    config.endpoints.max_response_body_bytes =
                        checked_number<std::size_t>(
                            require_unsigned(
                                *item, "endpoints.maxResponseBodyBytes"),
                            1024, 64 * 1024 * 1024,
                            "endpoints.maxResponseBodyBytes");
                if (const auto* item = endpoints.if_contains("maxMessageBytes"))
                    config.endpoints.max_message_bytes =
                        checked_number<std::size_t>(
                            require_unsigned(*item, "endpoints.maxMessageBytes"),
                            1024, 4 * 1024 * 1024,
                            "endpoints.maxMessageBytes");
                if (const auto* item = endpoints.if_contains("maxQueuedMessages"))
                    config.endpoints.max_queued_messages =
                        checked_number<std::size_t>(
                            require_unsigned(*item, "endpoints.maxQueuedMessages"),
                            1, 1024, "endpoints.maxQueuedMessages");
                if (const auto* item = endpoints.if_contains("maxQueuedBytes"))
                    config.endpoints.max_queued_bytes =
                        checked_number<std::size_t>(
                            require_unsigned(*item, "endpoints.maxQueuedBytes"),
                            1024, 64 * 1024 * 1024,
                            "endpoints.maxQueuedBytes");
                if (const auto* item = endpoints.if_contains("idleTimeoutMs"))
                    config.endpoints.idle_timeout = checked_milliseconds(
                        require_unsigned(*item, "endpoints.idleTimeoutMs"), 1000,
                        3600000, "endpoints.idleTimeoutMs");
                if (const auto* item = endpoints.if_contains("maxLifetimeMs"))
                    config.endpoints.max_lifetime = checked_milliseconds(
                        require_unsigned(*item, "endpoints.maxLifetimeMs"), 1000,
                        604800000, "endpoints.maxLifetimeMs");
            }
        }

        std::optional<std::string_view> inline_value(std::string_view argument,
            std::string_view option) {
            if (argument == option) return std::nullopt;
            if (argument.starts_with(option) && argument.size() > option.size() &&
                argument[option.size()] == '=')
                return argument.substr(option.size() + 1);
            return std::nullopt;
        }

        bool is_option(std::string_view argument, std::string_view option) {
            return argument == option ||
                (argument.starts_with(option) && argument.size() > option.size() &&
                    argument[option.size()] == '=');
        }

        std::string_view option_value(int& index, int argc, char** argv,
            std::string_view option) {
            const std::string_view argument(argv[index]);
            if (argument != option) {
                auto value = inline_value(argument, option);
                if (value && !value->empty()) return *value;
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            if (++index >= argc)
                throw std::runtime_error(std::string(option) + " requires a value");
            return argv[index];
        }

        void ensure_action(StartupAction current, StartupAction next) {
            if (current != StartupAction::run && current != next)
                throw std::runtime_error("only one command may be specified");
        }

        void validate_config(const CoreConfig& config) {
            if (config.state_dir.empty())
                throw std::runtime_error("stateDir must not be empty");
            if (config.public_dir.empty())
                throw std::runtime_error("web.publicDir must not be empty");
            boost::system::error_code address_error;
            (void)boost::asio::ip::make_address(
                config.listen_address, address_error);
            if (address_error)
                throw std::runtime_error(
                    "listen.address must be a numeric IPv4 or IPv6 address");
            if (config.auth.username.empty() || config.auth.username.size() > 128)
                throw std::runtime_error(
                    "auth.username must contain 1..128 bytes");
            for (const unsigned char byte : config.auth.username) {
                if (byte < 0x20 || byte == 0x7f)
                    throw std::runtime_error(
                        "auth.username must not contain control characters");
            }
            if (config.auth.password_hash.size() > 4096)
                throw std::runtime_error(
                    "auth.passwordHash must not exceed 4096 bytes");
            if (config.auth.password_hash.find_first_of("\r\n\0", 0, 3) !=
                std::string::npos)
                throw std::runtime_error(
                    "auth.passwordHash must be a single line");
            if (config.auth.enabled && config.auth.password_hash.empty())
                throw std::runtime_error(
                    "auth.passwordHash is required when auth is enabled");
            if (config.auth.enabled &&
                !AuthManager::valid_password_hash(config.auth.password_hash))
                throw std::runtime_error(
                    "auth.passwordHash is not a valid circud scrypt hash");
            if (config.auth.session_idle_timeout >
                config.auth.session_max_lifetime)
                throw std::runtime_error(
                    "auth.sessionIdleTimeoutMs must not exceed "
                    "auth.sessionMaxLifetimeMs");
            if (config.endpoints.max_sessions_per_package >
                config.endpoints.max_sessions) {
                throw std::runtime_error(
                    "endpoints.maxSessionsPerPackage must not exceed "
                    "endpoints.maxSessions");
            }
            if (config.endpoints.max_message_bytes >
                config.endpoints.max_queued_bytes) {
                throw std::runtime_error(
                    "endpoints.maxMessageBytes must not exceed "
                    "endpoints.maxQueuedBytes");
            }
            if (config.endpoints.idle_timeout > config.endpoints.max_lifetime) {
                throw std::runtime_error(
                    "endpoints.idleTimeoutMs must not exceed "
                    "endpoints.maxLifetimeMs");
            }
        }

    } // namespace

    StartupOptions parse_startup_options(int argc, char** argv,
        std::filesystem::path default_public_dir) {
        StartupOptions options;
        options.config.public_dir = std::move(default_public_dir);

        // Keep the password helper isolated from all ordinary options. In
        // particular, it has no argument or environment-based secret source.
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) != "hash-password") continue;
            if (index != 1 || argc != 2)
                throw std::runtime_error(
                    "hash-password does not accept options or arguments");
            options.action = StartupAction::hash_password;
            return options;
        }

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (!is_option(argument, "--config")) continue;
            if (!options.config_file.empty())
                throw std::runtime_error("--config may only be specified once");
            options.config_file = option_value(index, argc, argv, "--config");
        }
        if (!options.config_file.empty())
            apply_config_file(options.config_file, options.config);

        bool install_seen = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (is_option(argument, "--config")) {
                (void) option_value(index, argc, argv, "--config");
            }
            else if (is_option(argument, "--state-dir")) {
                options.config.state_dir = option_value(index, argc, argv, "--state-dir");
            }
            else if (is_option(argument, "--listen")) {
                auto [address, port] =
                    parse_listen(option_value(index, argc, argv, "--listen"));
                options.config.listen_address = std::move(address);
                options.config.listen_port = port;
            }
            else if (is_option(argument, "--public-dir")) {
                options.config.public_dir = option_value(index, argc, argv, "--public-dir");
            }
            else if (is_option(argument, "--http-background-workers")) {
                options.config.http_background_workers =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--http-background-workers"),
                            "--http-background-workers"),
                        1, 16, "--http-background-workers");
            }
            else if (is_option(argument, "--http-max-connections")) {
                options.config.http_max_connections = checked_number<std::size_t>(
                    parse_unsigned(option_value(index, argc, argv,
                        "--http-max-connections"),
                        "--http-max-connections"),
                    1, 4096, "--http-max-connections");
            }
            else if (is_option(argument, "--http-io-timeout-ms")) {
                options.config.http_io_timeout = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--http-io-timeout-ms"),
                        "--http-io-timeout-ms"),
                    100, 300000, "--http-io-timeout-ms");
            }
            else if (is_option(argument, "--http-max-body-bytes")) {
                options.config.http_max_body_bytes = checked_number<std::size_t>(
                    parse_unsigned(option_value(index, argc, argv,
                        "--http-max-body-bytes"),
                        "--http-max-body-bytes"),
                    1024, 64 * 1024, "--http-max-body-bytes");
            }
            else if (is_option(argument, "--http-max-upload-bytes")) {
                options.config.http_max_upload_bytes =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(
                            index, argc, argv, "--http-max-upload-bytes"),
                            "--http-max-upload-bytes"),
                        1024, 64 * 1024 * 1024,
                        "--http-max-upload-bytes");
            }
            else if (is_option(argument, "--http-max-static-file-bytes")) {
                options.config.http_max_static_file_bytes =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(
                            index, argc, argv,
                            "--http-max-static-file-bytes"),
                            "--http-max-static-file-bytes"),
                        1024, 64 * 1024 * 1024,
                        "--http-max-static-file-bytes");
            }
            else if (is_option(argument, "--status-interval-ms")) {
                options.config.status_interval = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--status-interval-ms"),
                        "--status-interval-ms"),
                    1000, 3600000, "--status-interval-ms");
            }
            else if (is_option(argument, "--status-timeout-ms")) {
                options.config.status_timeout = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--status-timeout-ms"),
                        "--status-timeout-ms"),
                    100, 300000, "--status-timeout-ms");
            }
            else if (is_option(argument, "--default-call-timeout-ms")) {
                options.config.default_call_timeout = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--default-call-timeout-ms"),
                        "--default-call-timeout-ms"),
                    1, 300000, "--default-call-timeout-ms");
            }
            else if (is_option(argument, "--runtime-start-timeout-ms")) {
                options.config.runtime_start_timeout = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--runtime-start-timeout-ms"),
                        "--runtime-start-timeout-ms"),
                    100, 300000, "--runtime-start-timeout-ms");
            }
            else if (is_option(argument, "--event-capacity")) {
                options.config.event_capacity = checked_number<std::size_t>(
                    parse_unsigned(option_value(index, argc, argv,
                        "--event-capacity"), "--event-capacity"),
                    32, 4096, "--event-capacity");
            }
            else if (is_option(argument, "--auth-username")) {
                options.config.auth.username = option_value(
                    index, argc, argv, "--auth-username");
            }
            else if (is_option(argument,
                         "--auth-session-idle-timeout-ms")) {
                options.config.auth.session_idle_timeout = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--auth-session-idle-timeout-ms"),
                        "--auth-session-idle-timeout-ms"),
                    1000, 86400000, "--auth-session-idle-timeout-ms");
            }
            else if (is_option(argument,
                         "--auth-session-max-lifetime-ms")) {
                options.config.auth.session_max_lifetime = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--auth-session-max-lifetime-ms"),
                        "--auth-session-max-lifetime-ms"),
                    1000, 2592000000ULL,
                    "--auth-session-max-lifetime-ms");
            }
            else if (is_option(argument, "--auth-max-sessions")) {
                options.config.auth.max_sessions = checked_number<std::size_t>(
                    parse_unsigned(option_value(index, argc, argv,
                        "--auth-max-sessions"), "--auth-max-sessions"),
                    1, 4096, "--auth-max-sessions");
            }
            else if (is_option(argument, "--endpoint-max-sessions")) {
                options.config.endpoints.max_sessions = checked_number<std::size_t>(
                    parse_unsigned(option_value(index, argc, argv,
                        "--endpoint-max-sessions"),
                        "--endpoint-max-sessions"),
                    1, 4096, "--endpoint-max-sessions");
            }
            else if (is_option(argument,
                         "--endpoint-max-sessions-per-package")) {
                options.config.endpoints.max_sessions_per_package =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--endpoint-max-sessions-per-package"),
                            "--endpoint-max-sessions-per-package"),
                        1, 1024, "--endpoint-max-sessions-per-package");
            }
            else if (is_option(argument,
                         "--endpoint-max-request-body-bytes")) {
                options.config.endpoints.max_request_body_bytes =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--endpoint-max-request-body-bytes"),
                            "--endpoint-max-request-body-bytes"),
                        1024, 16 * 1024 * 1024,
                        "--endpoint-max-request-body-bytes");
            }
            else if (is_option(argument,
                         "--endpoint-max-response-body-bytes")) {
                options.config.endpoints.max_response_body_bytes =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--endpoint-max-response-body-bytes"),
                            "--endpoint-max-response-body-bytes"),
                        1024, 64 * 1024 * 1024,
                        "--endpoint-max-response-body-bytes");
            }
            else if (is_option(argument, "--endpoint-max-message-bytes")) {
                options.config.endpoints.max_message_bytes =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--endpoint-max-message-bytes"),
                            "--endpoint-max-message-bytes"),
                        1024, 4 * 1024 * 1024,
                        "--endpoint-max-message-bytes");
            }
            else if (is_option(argument,
                         "--endpoint-max-queued-messages")) {
                options.config.endpoints.max_queued_messages =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--endpoint-max-queued-messages"),
                            "--endpoint-max-queued-messages"),
                        1, 1024, "--endpoint-max-queued-messages");
            }
            else if (is_option(argument, "--endpoint-max-queued-bytes")) {
                options.config.endpoints.max_queued_bytes =
                    checked_number<std::size_t>(
                        parse_unsigned(option_value(index, argc, argv,
                            "--endpoint-max-queued-bytes"),
                            "--endpoint-max-queued-bytes"),
                        1024, 64 * 1024 * 1024,
                        "--endpoint-max-queued-bytes");
            }
            else if (is_option(argument, "--endpoint-idle-timeout-ms")) {
                options.config.endpoints.idle_timeout = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--endpoint-idle-timeout-ms"),
                        "--endpoint-idle-timeout-ms"),
                    1000, 3600000, "--endpoint-idle-timeout-ms");
            }
            else if (is_option(argument, "--endpoint-max-lifetime-ms")) {
                options.config.endpoints.max_lifetime = checked_milliseconds(
                    parse_unsigned(option_value(index, argc, argv,
                        "--endpoint-max-lifetime-ms"),
                        "--endpoint-max-lifetime-ms"),
                    1000, 604800000, "--endpoint-max-lifetime-ms");
            }
            else if (argument == "--no-web") {
                options.config.web_enabled = false;
            }
            else if (argument == "--web") {
                options.config.web_enabled = true;
            }
            else if (argument == "--no-status-polling") {
                options.config.status_polling_enabled = false;
            }
            else if (argument == "--status-polling") {
                options.config.status_polling_enabled = true;
            }
            else if (argument == "--no-events") {
                options.config.events_enabled = false;
            }
            else if (argument == "--events") {
                options.config.events_enabled = true;
            }
            else if (argument == "--no-auth") {
                options.config.auth.enabled = false;
            }
            else if (argument == "--auth") {
                options.config.auth.enabled = true;
            }
            else if (argument == "--auth-secure-cookie") {
                options.config.auth.secure_cookie = true;
            }
            else if (argument == "--auth-insecure-cookie") {
                options.config.auth.secure_cookie = false;
            }
            else if (argument == "--no-endpoints") {
                options.config.endpoints.enabled = false;
            }
            else if (argument == "--endpoints") {
                options.config.endpoints.enabled = true;
            }
            else if (argument == "--check-config") {
                ensure_action(options.action, StartupAction::check_config);
                options.action = StartupAction::check_config;
            }
            else if (argument == "--print-config") {
                ensure_action(options.action, StartupAction::print_config);
                options.action = StartupAction::print_config;
            }
            else if (argument == "--help" || argument == "-h") {
                ensure_action(options.action, StartupAction::help);
                options.action = StartupAction::help;
            }
            else if (argument == "--version") {
                ensure_action(options.action, StartupAction::version);
                options.action = StartupAction::version;
            }
            else if (argument == "install") {
                ensure_action(options.action, StartupAction::install);
                options.action = StartupAction::install;
                if (install_seen) throw std::runtime_error("install specified twice");
                install_seen = true;
            }
            else if (install_seen && options.install_archive.empty()) {
                options.install_archive = argument;
            }
            else {
                throw std::runtime_error("unknown argument: " + std::string(argument));
            }
        }
        if (options.action == StartupAction::install && options.install_archive.empty())
            throw std::runtime_error("install requires a package ZIP path");
        validate_config(options.config);
        if (options.action == StartupAction::check_config &&
            options.config.web_enabled) {
            std::error_code public_error;
            const bool public_is_directory = std::filesystem::is_directory(
                options.config.public_dir, public_error);
            if (public_error || !public_is_directory)
                throw std::runtime_error(
                    "web.publicDir must name an existing directory when Web is enabled");
        }
        return options;
    }

    std::string serialize_config(const CoreConfig& config) {
        return serialize_json(boost::json::object{
            { "stateDir", config.state_dir.string() },
            { "listen", boost::json::object{ { "address", config.listen_address },
                                            { "port", config.listen_port } } },
            { "web", boost::json::object{
                        { "enabled", config.web_enabled },
                        { "publicDir", config.public_dir.string() },
                        { "backgroundWorkers", config.http_background_workers },
                        { "maxConnections", config.http_max_connections },
                        { "ioTimeoutMs", config.http_io_timeout.count() },
                        { "maxBodyBytes", config.http_max_body_bytes },
                        { "maxUploadBytes", config.http_max_upload_bytes },
                        { "maxStaticFileBytes", config.http_max_static_file_bytes } } },
            { "status", boost::json::object{
                           { "enabled", config.status_polling_enabled },
                           { "intervalMs", config.status_interval.count() },
                           { "timeoutMs", config.status_timeout.count() } } },
            { "calls", boost::json::object{
                          { "defaultTimeoutMs", config.default_call_timeout.count() } } },
            { "runtimeStartTimeoutMs", config.runtime_start_timeout.count() },
            { "events", boost::json::object{
                           { "enabled", config.events_enabled },
                           { "capacity", config.event_capacity } } },
            { "auth", boost::json::object{
                         { "enabled", config.auth.enabled },
                         { "username", config.auth.username },
                         { "passwordHash", config.auth.password_hash.empty()
                               ? ""
                               : "<redacted>" },
                         { "sessionIdleTimeoutMs",
                           config.auth.session_idle_timeout.count() },
                         { "sessionMaxLifetimeMs",
                           config.auth.session_max_lifetime.count() },
                         { "maxSessions", config.auth.max_sessions },
                         { "secureCookie", config.auth.secure_cookie } } },
            { "endpoints", boost::json::object{
                              { "enabled", config.endpoints.enabled },
                              { "maxSessions", config.endpoints.max_sessions },
                              { "maxSessionsPerPackage",
                                config.endpoints.max_sessions_per_package },
                              { "maxRequestBodyBytes",
                                config.endpoints.max_request_body_bytes },
                              { "maxResponseBodyBytes",
                                config.endpoints.max_response_body_bytes },
                              { "maxMessageBytes",
                                config.endpoints.max_message_bytes },
                              { "maxQueuedMessages",
                                config.endpoints.max_queued_messages },
                              { "maxQueuedBytes",
                                config.endpoints.max_queued_bytes },
                              { "idleTimeoutMs",
                                config.endpoints.idle_timeout.count() },
                              { "maxLifetimeMs",
                                config.endpoints.max_lifetime.count() } } },
            });
    }

    std::string usage_text(std::string_view executable) {
        std::ostringstream output;
        output << "Usage: " << executable << " [OPTIONS]\n"
            << "       " << executable << " [OPTIONS] install PACKAGE.zip\n\n"
            << "       " << executable << " hash-password\n\n"
            << "General:\n"
            << "  --config FILE                    Load JSON configuration\n"
            << "  --state-dir PATH                 Persistent state directory\n"
            << "  --listen ADDRESS:PORT            HTTP listen endpoint\n"
            << "  --public-dir PATH                Static Web root\n"
            << "  --web | --no-web                 Enable or disable HTTP\n"
            << "  --check-config                   Validate configuration and exit\n"
            << "  --print-config                   Print effective JSON and exit\n"
            << "  -h, --help                       Show this help\n"
            << "  --version                        Show version\n\n"
            << "HTTP limits:\n"
            << "  --http-background-workers N\n"
            << "  --http-max-connections N\n"
            << "  --http-io-timeout-ms N\n"
            << "  --http-max-body-bytes N\n"
            << "  --http-max-upload-bytes N\n"
            << "  --http-max-static-file-bytes N\n\n"
            << "Package endpoints:\n"
            << "  --endpoints | --no-endpoints\n"
            << "  --endpoint-max-sessions N\n"
            << "  --endpoint-max-sessions-per-package N\n"
            << "  --endpoint-max-request-body-bytes N\n"
            << "  --endpoint-max-response-body-bytes N\n"
            << "  --endpoint-max-message-bytes N\n"
            << "  --endpoint-max-queued-messages N\n"
            << "  --endpoint-max-queued-bytes N\n"
            << "  --endpoint-idle-timeout-ms N\n"
            << "  --endpoint-max-lifetime-ms N\n\n"
            << "Calls and status:\n"
            << "  --default-call-timeout-ms N\n"
            << "  --runtime-start-timeout-ms N\n"
            << "  --status-interval-ms N\n"
            << "  --status-timeout-ms N\n"
            << "  --status-polling | --no-status-polling\n"
            << "  --event-capacity N              In-memory event capacity (32..4096)\n"
            << "  --events | --no-events          Enable or disable management events\n\n"
            << "Authentication (passwordHash is config-file only):\n"
            << "  hash-password                    Read twice from a TTY without echo\n"
            << "                                   and print a configuration hash; no\n"
            << "                                   password argument or pipe is accepted\n"
            << "  --auth | --no-auth\n"
            << "  --auth-username NAME\n"
            << "  --auth-session-idle-timeout-ms N\n"
            << "  --auth-session-max-lifetime-ms N\n"
            << "  --auth-max-sessions N\n"
            << "  --auth-secure-cookie | --auth-insecure-cookie\n";
        return output.str();
    }

} // namespace circud
