#include <circud/auth.hpp>
#include <circud/config.hpp>
#include <circud/core.hpp>
#include <circud/http_server.hpp>

#include "tjs.h"

#include <boost/asio/signal_set.hpp>

#include <openssl/crypto.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <termios.h>
#include <unistd.h>

#ifndef CIRCUD_VERSION
#define CIRCUD_VERSION "unknown"
#endif

#ifndef CIRCUD_INSTALL_PUBLIC_DIR
#define CIRCUD_INSTALL_PUBLIC_DIR "/usr/share/circud/public"
#endif

#ifndef CIRCUD_SOURCE_PUBLIC_DIR
#define CIRCUD_SOURCE_PUBLIC_DIR "public"
#endif

namespace {

    class SecretString {
    public:
        SecretString() { value.reserve(1024); }

        ~SecretString() {
            if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
        }

        std::string value;
    };

    class SignalBlock {
    public:
        SignalBlock() {
            sigfillset(&blocked_);
            if (sigprocmask(SIG_BLOCK, &blocked_, &original_) != 0)
                throw std::runtime_error(
                    "cannot protect password prompt from signals");
            active_ = true;
        }

        ~SignalBlock() {
            if (active_) (void)sigprocmask(SIG_SETMASK, &original_, nullptr);
        }

        SignalBlock(const SignalBlock&) = delete;
        SignalBlock& operator=(const SignalBlock&) = delete;

    private:
        sigset_t blocked_{};
        sigset_t original_{};
        bool active_ = false;
    };

    class TerminalNoEcho {
    public:
        TerminalNoEcho() {
            if (!isatty(STDIN_FILENO))
                throw std::runtime_error(
                    "hash-password requires a TTY on standard input");
            if (tcgetattr(STDIN_FILENO, &original_) != 0)
                throw std::runtime_error("cannot inspect terminal settings");
            auto hidden = original_;
            hidden.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL));
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0)
                throw std::runtime_error("cannot disable terminal echo");
            active_ = true;
        }

        ~TerminalNoEcho() {
            if (active_)
                (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }

        TerminalNoEcho(const TerminalNoEcho&) = delete;
        TerminalNoEcho& operator=(const TerminalNoEcho&) = delete;

    private:
        termios original_{};
        bool active_ = false;
    };

    void read_password_line(SecretString& secret) {
        constexpr std::size_t max_password_bytes = 1024;
        bool too_long = false;
        while (true) {
            char byte = '\0';
            const auto received = ::read(STDIN_FILENO, &byte, 1);
            if (received == 0)
                throw std::runtime_error("terminal closed while reading password");
            if (received < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("cannot read password from terminal");
            }
            if (byte == '\n') break;
            if (secret.value.size() == max_password_bytes) {
                too_long = true;
                continue;
            }
            secret.value.push_back(byte);
        }
        if (too_long || secret.value.empty())
            throw std::runtime_error("password must contain 1..1024 bytes");
    }

    std::string password_hash_from_tty() {
        SecretString password;
        SecretString confirmation;
        {
            // Destruction is reverse order: terminal echo is restored before
            // blocked termination and stop signals are delivered.
            SignalBlock signals;
            TerminalNoEcho terminal;
            try {
                std::cerr << "Password: " << std::flush;
                read_password_line(password);
                std::cerr << "\nConfirm password: " << std::flush;
                read_password_line(confirmation);
                std::cerr << '\n';
            }
            catch (...) {
                std::cerr << '\n';
                throw;
            }
        }

        if (password.value != confirmation.value)
            throw std::runtime_error("password confirmation does not match");
        return circud::AuthManager::hash_password(password.value);
    }

    std::filesystem::path default_public_dir(const char* executable) {
        std::error_code error;
        auto binary = std::filesystem::weakly_canonical(executable, error);
        if (!error) {
            const auto root = binary.parent_path().parent_path();
            const auto installed = root / "share/circud/public";
            if (std::filesystem::is_directory(installed, error)) return installed;
            error.clear();
            const auto source_tree = root / "public";
            if (std::filesystem::is_directory(source_tree, error)) return source_tree;
        }
        error.clear();
        if (std::filesystem::is_directory(CIRCUD_INSTALL_PUBLIC_DIR, error))
            return CIRCUD_INSTALL_PUBLIC_DIR;
        return CIRCUD_SOURCE_PUBLIC_DIR;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        auto options = circud::parse_startup_options(
            argc, argv, default_public_dir(argv[0]));
        if (options.action == circud::StartupAction::help) {
            std::cout << circud::usage_text(argv[0]);
            return 0;
        }
        if (options.action == circud::StartupAction::version) {
            std::cout << "circud " << CIRCUD_VERSION << '\n';
            return 0;
        }
        if (options.action == circud::StartupAction::check_config) {
            std::cout << "configuration is valid\n";
            return 0;
        }
        if (options.action == circud::StartupAction::print_config) {
            std::cout << circud::serialize_config(options.config) << '\n';
            return 0;
        }
        if (options.action == circud::StartupAction::hash_password) {
            std::cout << password_hash_from_tty() << '\n';
            return 0;
        }

        TJS_Initialize(argc, argv);
        circud::Core core(options.config);
        if (options.action == circud::StartupAction::install) {
            auto package = core.install(options.install_archive);
            std::cout << "installed " << package.manifest.id << ' '
                << package.manifest.version << '\n';
            return 0;
        }

        std::unique_ptr<circud::HttpServer> server;
        try {
            // Keep every daemon-running path inside this boundary. Core::start()
            // can publish work before a later allocation reports failure, so
            // even an exception from start itself must use the bounded daemon
            // exit path rather than unwinding through Core's conservative
            // embedding destructor.
            core.start();
            if (options.config.web_enabled) {
                server = std::make_unique<circud::HttpServer>(core);
                server->start();
                const auto address = options.config.listen_address.find(':') ==
                    std::string::npos
                    ? options.config.listen_address
                    : '[' + options.config.listen_address + ']';
                std::cout << "circud listening on http://" << address << ':'
                    << options.config.listen_port << "/\n";
            }
            else {
                std::cout << "circud started with HTTP disabled\n";
            }

            boost::asio::signal_set signals(core.io(), SIGINT, SIGTERM);
            signals.async_wait([&](const boost::system::error_code& error, int) {
                if (error) return;
                if (server) server->stop();
                core.stop();
            });
            core.io().run();
        }
        catch (...) {
            const auto failure = std::current_exception();
            try {
                std::rethrow_exception(failure);
            }
            catch (const std::exception& exception) {
                std::cerr << "circud: " << exception.what() << '\n';
            }
            catch (...) {
                std::cerr << "circud: unknown daemon failure\n";
            }
            std::cerr.flush();

            // HttpServer::stop() only revokes admission and schedules socket
            // closure; it does not join the filesystem/background executor.
            // Core::stop() similarly revokes every package generation and
            // bounds graceful package shutdown by shutdownTimeoutMs. If either
            // submission itself fails (for example under allocation failure),
            // exiting immediately is safer than entering an unbounded
            // destructor or leaving a work guard alive forever.
            try {
                if (server) server->stop();
            }
            catch (...) {}
            bool stop_submitted = false;
            try {
                core.stop();
                stop_submitted = true;
            }
            catch (...) {}
            if (stop_submitted) {
                try {
                    if (core.io().stopped()) core.io().restart();
                    core.io().run();
                }
                catch (...) {
                    // A second control-handler failure must still terminate
                    // without unwinding into joins owned by conservative
                    // embedding/test destruction.
                }
            }
            std::cout.flush();
            std::cerr.flush();
            std::_Exit(EXIT_FAILURE);
        }
        if (server) server->stop();
        core.stop();
        std::cout.flush();
        std::cerr.flush();
        // A trusted native-capable package can block forever inside FFI. The
        // network and control actors have completed their bounded shutdown
        // here. Do not destroy their pools or re-enter Core's conservative
        // embedding/test destructor: either could wait indefinitely for an
        // unkillable package owner or blocking task.
        std::_Exit(EXIT_SUCCESS);
    }
    catch (const std::exception& exception) {
        std::cerr << "circud: " << exception.what() << '\n';
        return 1;
    }
}
