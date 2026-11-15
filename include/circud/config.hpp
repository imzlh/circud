#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace circud {

    struct AuthConfig {
        // Authentication gates core/management APIs. Static Web assets stay
        // public; package endpoints receive authorization state and decide.
        bool enabled = false;
        std::string username = "admin";
        std::string password_hash;
        std::chrono::milliseconds session_idle_timeout{ 30 * 60 * 1000 };
        std::chrono::milliseconds session_max_lifetime{ 24 * 60 * 60 * 1000 };
        std::size_t max_sessions = 64;
        bool secure_cookie = false;
    };

    struct EndpointConfig {
        bool enabled = true;
        std::size_t max_sessions = 128;
        std::size_t max_sessions_per_package = 32;
        std::size_t max_request_body_bytes = 1024 * 1024;
        std::size_t max_response_body_bytes = 4 * 1024 * 1024;
        std::size_t max_message_bytes = 256 * 1024;
        std::size_t max_queued_messages = 64;
        std::size_t max_queued_bytes = 1024 * 1024;
        std::chrono::milliseconds idle_timeout{ 60000 };
        std::chrono::milliseconds max_lifetime{ 24 * 60 * 60 * 1000 };
    };

    struct CoreConfig {
        std::filesystem::path state_dir = "/var/lib/circud";
        std::string listen_address = "127.0.0.1";
        std::uint16_t listen_port = 9080;
        bool web_enabled = true;
        std::filesystem::path public_dir = "public";
        std::size_t http_background_workers = 2;
        std::size_t http_max_connections = 128;
        std::chrono::milliseconds http_io_timeout{ 30000 };
        std::size_t http_max_body_bytes = 64 * 1024;
        std::size_t http_max_upload_bytes = 64 * 1024 * 1024;
        std::size_t http_max_static_file_bytes = 4 * 1024 * 1024;
        bool status_polling_enabled = true;
        std::chrono::milliseconds status_interval{ 30000 };
        std::chrono::milliseconds status_timeout{ 5000 };
        std::chrono::milliseconds default_call_timeout{ 30000 };
        std::chrono::milliseconds runtime_start_timeout{ 10000 };
        bool events_enabled = true;
        std::size_t event_capacity = 256;
        AuthConfig auth;
        EndpointConfig endpoints;
    };

    enum class StartupAction {
        run,
        install,
        check_config,
        print_config,
        hash_password,
        help,
        version,
    };

    struct StartupOptions {
        CoreConfig config;
        StartupAction action = StartupAction::run;
        std::filesystem::path install_archive;
        std::filesystem::path config_file;
    };

    StartupOptions parse_startup_options(int argc, char** argv,
        std::filesystem::path default_public_dir);
    std::string serialize_config(const CoreConfig& config);
    std::string usage_text(std::string_view executable);

} // namespace circud
