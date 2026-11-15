#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace circud {

    inline constexpr std::size_t kMaxEndpointEnvelopeBytes = 64 * 1024;
    inline constexpr std::size_t kMaxEndpointCommands = 64;
    inline constexpr std::size_t kMaxEndpointCommandBytes = 512 * 1024;

    enum class EndpointProtocol {
        http,
        sse,
        websocket,
    };

    using EndpointHeaders =
        std::vector<std::pair<std::string, std::string>>;
    using EndpointBytes = std::vector<std::byte>;

    // All endpoint values are native-owned. They may cross threads; none may
    // contain a JSContext, JSValue, or storage borrowed from an HTTP parser.
    struct EndpointRequest {
        std::uint64_t id = 0;
        EndpointProtocol protocol = EndpointProtocol::http;
        std::string method;
        std::string path;
        std::string query;
        EndpointHeaders headers;
        EndpointBytes body;
        std::vector<std::string> offered_subprotocols;
        std::string remote_address;
        // Authentication is advisory for package-owned scopes. The core
        // never rejects a package request solely for being anonymous; the
        // package chooses its own 401/403/public policy.
        bool is_authorized = false;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct EndpointResponse {
        std::uint64_t id = 0;
        unsigned status = 200;
        EndpointHeaders headers;
        EndpointBytes body;
    };

    struct EndpointAccept {
        std::uint64_t id = 0;
        EndpointProtocol protocol = EndpointProtocol::sse;
        unsigned status = 200;
        EndpointHeaders headers;
        std::string subprotocol;
    };

    // One-shot instruction for the network layer to serve a path relative to
    // the installed package version directory. The host bridge never reads it.
    struct EndpointStatic {
        std::uint64_t id = 0;
        std::string relative_path;
    };

    // Package-to-client output and WebSocket client-to-package input use the
    // same owned message envelope. SSE output is already SSE-framed UTF-8 and
    // always has binary == false.
    struct EndpointMessage {
        std::uint64_t id = 0;
        EndpointBytes data;
        bool binary = false;
    };

    // EndpointClose is terminal. Code 1000 is a normal stream/socket close;
    // 1001 is used when a package runtime stops, and 1011 for bridge failures.
    struct EndpointClose {
        std::uint64_t id = 0;
        std::uint16_t code = 1000;
        std::string reason;
    };

    using EndpointOutput = std::variant<EndpointResponse, EndpointAccept,
        EndpointStatic, EndpointMessage, EndpointClose>;

    class EndpointSink {
    public:
        virtual ~EndpointSink() = default;

        // Called on a package runtime thread. Implementations must be
        // thread-safe, non-blocking, and decide their queue bound before
        // returning. false means closed or backpressured; a false result is
        // treated as terminal by the package bridge.
        virtual bool emit(EndpointOutput output) = 0;
    };

} // namespace circud
