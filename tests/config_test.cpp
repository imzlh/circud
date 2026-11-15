#include "test.hpp"

#include <circud/config.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::string_view valid_password_hash =
    "$circud-scrypt$v=1$ln=14,r=8,p=1$"
    "AAAAAAAAAAAAAAAAAAAAAA$"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("circud-config-test-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path);
    if (!stream) throw std::runtime_error("cannot create config test file");
    stream << text;
}

circud::StartupOptions parse(std::vector<std::string> arguments,
                             const std::filesystem::path& public_dir) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) argv.push_back(argument.data());
    return circud::parse_startup_options(static_cast<int>(argv.size()),
                                         argv.data(), public_dir);
}

} // namespace

CIRCUD_TEST(config_file_loads_and_command_line_overrides) {
    TempDir temp;
    const auto path = temp.path / "circud.json";
    write_file(path, R"({
      "stateDir":"data",
      "listen":{"address":"::1","port":8000},
      "web":{"publicDir":"assets","backgroundWorkers":2,"maxConnections":90,
             "ioTimeoutMs":1500,"maxBodyBytes":4096,
             "maxUploadBytes":1048576,"maxStaticFileBytes":8192},
      "status":{"enabled":false,"intervalMs":2000,"timeoutMs":300},
      "calls":{"defaultTimeoutMs":700},
      "events":{"enabled":false,"capacity":64},
      "auth":{"enabled":true,"username":"operator",
              "passwordHash":"$circud-scrypt$v=1$ln=14,r=8,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
              "sessionIdleTimeoutMs":60000,
              "sessionMaxLifetimeMs":3600000,
              "maxSessions":20,"secureCookie":true},
      "endpoints":{"enabled":false,"maxSessions":80,
                   "maxSessionsPerPackage":20,"maxRequestBodyBytes":8192,
                   "maxResponseBodyBytes":16384,"maxMessageBytes":2048,
                   "maxQueuedMessages":12,"maxQueuedBytes":4096,
                   "idleTimeoutMs":4000,"maxLifetimeMs":5000}
    })");
    const auto options = parse(
        {"circud", "--config", path.string(), "--listen", "[::1]:9001",
         "--http-background-workers=3", "--http-max-connections=100",
         "--status-polling", "--events", "--endpoints",
         "--event-capacity=128", "--endpoint-max-sessions=96",
         "--auth-username=admin2", "--auth-max-sessions=24",
         "--auth-session-idle-timeout-ms=120000",
         "--auth-session-max-lifetime-ms=7200000",
         "--auth-insecure-cookie"},
        temp.path / "fallback-public");
    circud::test::require(options.config.state_dir == temp.path / "data");
    circud::test::require(options.config.public_dir == temp.path / "assets");
    circud::test::require(options.config.listen_address == "::1");
    circud::test::require(options.config.listen_port == 9001);
    circud::test::require(options.config.http_background_workers == 3);
    circud::test::require(options.config.http_max_connections == 100);
    circud::test::require(options.config.http_max_upload_bytes == 1048576);
    circud::test::require(options.config.status_polling_enabled);
    circud::test::require(options.config.default_call_timeout.count() == 700);
    circud::test::require(options.config.events_enabled);
    circud::test::require(options.config.event_capacity == 128);
    circud::test::require(options.config.auth.enabled);
    circud::test::require(options.config.auth.username == "admin2");
    circud::test::require(options.config.auth.password_hash ==
        valid_password_hash);
    circud::test::require(
        options.config.auth.session_idle_timeout.count() == 120000);
    circud::test::require(
        options.config.auth.session_max_lifetime.count() == 7200000);
    circud::test::require(options.config.auth.max_sessions == 24);
    circud::test::require(!options.config.auth.secure_cookie);
    circud::test::require(options.config.endpoints.enabled);
    circud::test::require(options.config.endpoints.max_sessions == 96);
    circud::test::require(
        options.config.endpoints.max_sessions_per_package == 20);
    circud::test::require(
        options.config.endpoints.max_request_body_bytes == 8192);
    circud::test::require(options.config.endpoints.max_queued_messages == 12);
    circud::test::require(options.config.endpoints.idle_timeout.count() == 4000);
}

CIRCUD_TEST(config_rejects_unknown_fields) {
    TempDir temp;
    const auto path = temp.path / "circud.json";
    write_file(path, R"({"web":{"workerz":4}})");
    bool rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "unknown config field was accepted");
}

CIRCUD_TEST(config_rejects_out_of_range_limits) {
    bool rejected = false;
    try {
        (void)parse({"circud", "--http-background-workers", "0"}, "public");
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "zero background workers were accepted");

    rejected = false;
    try {
        (void)parse({"circud", "--event-capacity", "31"}, "public");
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "undersized event capacity was accepted");
}

CIRCUD_TEST(config_runtime_start_timeout_parses_json_and_cli_override) {
    TempDir temp;
    const auto path = temp.path / "circud.json";
    write_file(path, R"({"runtimeStartTimeoutMs":1234})");

    const auto from_json = parse(
        {"circud", "--config", path.string()}, temp.path);
    circud::test::require(
        from_json.config.runtime_start_timeout.count() == 1234,
        "runtime start timeout was not loaded from JSON");

    const auto overridden = parse(
        {"circud", "--config", path.string(),
         "--runtime-start-timeout-ms=5678"},
        temp.path);
    circud::test::require(
        overridden.config.runtime_start_timeout.count() == 5678,
        "runtime start timeout CLI option did not override JSON");
}

CIRCUD_TEST(config_runtime_start_timeout_rejects_out_of_range_values) {
    TempDir temp;
    const auto path = temp.path / "circud.json";

    for (const auto value : {"99", "300001"}) {
        write_file(path,
            std::string("{\"runtimeStartTimeoutMs\":") + value + "}");
        bool rejected = false;
        try {
            (void)parse({"circud", "--config", path.string()}, temp.path);
        } catch (const std::exception&) {
            rejected = true;
        }
        circud::test::require(rejected,
            "out-of-range JSON runtime start timeout was accepted");
    }

    for (const auto value : {"99", "300001"}) {
        bool rejected = false;
        try {
            (void)parse(
                {"circud", "--runtime-start-timeout-ms", value},
                temp.path);
        } catch (const std::exception&) {
            rejected = true;
        }
        circud::test::require(rejected,
            "out-of-range CLI runtime start timeout was accepted");
    }
}

CIRCUD_TEST(config_runtime_start_timeout_serializes_and_round_trips) {
    TempDir temp;
    circud::CoreConfig config;
    config.runtime_start_timeout = std::chrono::milliseconds(54321);
    const auto text = circud::serialize_config(config);
    circud::test::require(
        text.find("\"runtimeStartTimeoutMs\":54321") != std::string::npos,
        "serialized config omitted the runtime start timeout key");

    const auto path = temp.path / "circud.json";
    write_file(path, text);
    const auto round_trip = parse(
        {"circud", "--config", path.string()}, temp.path);
    circud::test::require(
        round_trip.config.runtime_start_timeout.count() == 54321,
        "runtime start timeout did not survive config serialization");
}

CIRCUD_TEST(config_rejects_retired_worker_pool_fields) {
    TempDir temp;
    auto path = temp.path / "circud.json";
    write_file(path, R"({"web":{"workers":4}})");
    bool rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "retired web.workers was accepted");

    rejected = false;
    try {
        (void)parse({"circud", "--http-queue-limit", "32"}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "retired HTTP queue option was accepted");
}

CIRCUD_TEST(config_rejects_inconsistent_endpoint_limits) {
    TempDir temp;
    const auto path = temp.path / "circud.json";
    write_file(path,
        R"({"endpoints":{"maxSessions":8,"maxSessionsPerPackage":9}})");
    bool rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "per-package endpoint sessions exceeded global sessions");

    rejected = false;
    try {
        (void)parse({"circud", "--endpoint-max-message-bytes", "4096",
                       "--endpoint-max-queued-bytes", "2048"},
            temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "endpoint message limit exceeded queued byte limit");
}

CIRCUD_TEST(config_serialization_contains_modern_network_limits) {
    circud::CoreConfig config;
    config.http_background_workers = 3;
    config.http_max_connections = 321;
    config.endpoints.max_sessions = 44;
    const auto text = circud::serialize_config(config);
    circud::test::require(text.find("backgroundWorkers") != std::string::npos);
    circud::test::require(text.find("maxConnections") != std::string::npos);
    circud::test::require(text.find("maxSessions") != std::string::npos);
    circud::test::require(text.find("queueLimit") == std::string::npos);
    circud::test::require(text.find("\"workers\"") == std::string::npos);
}

CIRCUD_TEST(config_auth_defaults_are_disabled_and_bounded) {
    const auto options = parse({"circud"}, "public");
    circud::test::require(!options.config.auth.enabled);
    circud::test::require(options.config.auth.username == "admin");
    circud::test::require(options.config.auth.password_hash.empty());
    circud::test::require(
        options.config.auth.session_idle_timeout.count() == 1800000);
    circud::test::require(
        options.config.auth.session_max_lifetime.count() == 86400000);
    circud::test::require(options.config.auth.max_sessions == 64);
    circud::test::require(!options.config.auth.secure_cookie);
}

CIRCUD_TEST(config_auth_rejects_unknown_and_invalid_values) {
    TempDir temp;
    auto path = temp.path / "circud.json";
    write_file(path, R"({"auth":{"password":"secret"}})");
    bool rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "unknown auth field was accepted");

    write_file(path, R"({"auth":{"enabled":true}})");
    rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "enabled authentication without passwordHash was accepted");

    write_file(path,
        R"({"auth":{"enabled":true,"passwordHash":"$argon2id$invalid"}})");
    rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "enabled authentication with a foreign hash was accepted");

    write_file(path, R"({"auth":{"sessionIdleTimeoutMs":2000,
                                  "sessionMaxLifetimeMs":1000}})");
    rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "auth idle timeout exceeded maximum lifetime");

    write_file(path, R"({"auth":{"username":"bad\nname"}})");
    rejected = false;
    try {
        (void)parse({"circud", "--config", path.string()}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "auth username containing a control character was accepted");
}

CIRCUD_TEST(config_rejects_non_numeric_listen_addresses_and_empty_paths) {
    bool rejected = false;
    try {
        (void)parse({"circud", "--listen", "localhost:9080"}, "public");
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "a DNS listen address was accepted as a numeric address");

    rejected = false;
    try {
        (void)parse({"circud", "--state-dir", ""}, "public");
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "an empty state directory was accepted");

    rejected = false;
    try {
        (void)parse({"circud", "--public-dir", ""}, "public");
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "an empty public directory was accepted");
}

CIRCUD_TEST(check_config_requires_web_public_directory_only_when_enabled) {
    TempDir temp;
    const auto missing = temp.path / "missing-public";
    bool rejected = false;
    try {
        (void)parse({"circud", "--public-dir", missing.string(),
                       "--check-config"}, temp.path);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "--check-config accepted a missing enabled Web public directory");

    const auto disabled = parse({"circud", "--public-dir", missing.string(),
                                    "--no-web", "--check-config"},
        temp.path);
    circud::test::require(!disabled.config.web_enabled,
        "disabled Web unexpectedly required a public directory");

    const auto public_dir = temp.path / "public";
    std::filesystem::create_directory(public_dir);
    const auto enabled = parse({"circud", "--public-dir", public_dir.string(),
                                   "--check-config"},
        temp.path);
    circud::test::require(enabled.config.public_dir == public_dir,
        "--check-config rejected an existing Web public directory");
}

CIRCUD_TEST(config_auth_serialization_redacts_password_hash) {
    circud::CoreConfig config;
    config.auth.enabled = true;
    config.auth.password_hash = "never-print-this-hash";
    const auto text = circud::serialize_config(config);
    circud::test::require(text.find("never-print-this-hash") ==
        std::string::npos);
    circud::test::require(text.find("\"passwordHash\":\"<redacted>\"") !=
        std::string::npos);
    circud::test::require(text.find("\"auth\"") != std::string::npos);
    circud::test::require(text.find("\"username\":\"admin\"") !=
        std::string::npos);
}

CIRCUD_TEST(config_auth_has_no_password_cli_option) {
    bool rejected = false;
    try {
        (void)parse({"circud", "--auth-password-hash", "secret"}, "public");
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
        "password hash was accepted from the command line");
}

CIRCUD_TEST(config_parses_install_after_options) {
    const auto options = parse(
        {"circud", "--state-dir", "state", "install", "demo.zip"}, "public");
    circud::test::require(options.action == circud::StartupAction::install);
    circud::test::require(options.install_archive == "demo.zip");
    circud::test::require(options.config.state_dir == "state");
}

CIRCUD_TEST(config_parses_isolated_hash_password_command) {
    const auto options = parse({"circud", "hash-password"}, "public");
    circud::test::require(options.action ==
        circud::StartupAction::hash_password);

    bool secret_rejected = false;
    try {
        (void)parse({"circud", "hash-password", "secret"}, "public");
    } catch (const std::exception&) {
        secret_rejected = true;
    }
    circud::test::require(secret_rejected,
        "hash-password accepted a password argument");

    bool option_rejected = false;
    try {
        (void)parse({"circud", "--config", "missing.json", "hash-password"},
            "public");
    } catch (const std::exception&) {
        option_rejected = true;
    }
    circud::test::require(option_rejected,
        "hash-password accepted ordinary daemon options");
}

CIRCUD_TEST(config_help_documents_tty_only_password_hashing) {
    const auto help = circud::usage_text("circud");
    circud::test::require(help.find("circud hash-password") !=
        std::string::npos);
    circud::test::require(help.find("TTY without echo") != std::string::npos);
    circud::test::require(help.find("password argument or pipe is accepted") !=
        std::string::npos);
}
