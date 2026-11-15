#pragma once

#include <circud/http_background.hpp>
#include <circud/package_endpoint.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace circud {

    class Core;

    // Owns one package endpoint connection after the HTTP listener has read
    // its request. All socket work is serialized on Core::io(); emit() is the
    // only entry point used from a package thread and never blocks.
    class EndpointSession final : public EndpointSink,
                                  public std::enable_shared_from_this<
                                      EndpointSession> {
    public:
        // A completed ordinary HTTP response may return the transport to the
        // listener. The buffer is part of the handoff because Beast can read
        // pipelined request bytes while parsing the current request.
        struct Handoff {
            boost::beast::tcp_stream stream;
            boost::beast::flat_buffer buffer;
        };

        using Completion =
            std::function<void(std::optional<Handoff> handoff)>;

        EndpointSession(Core& core,
            std::shared_ptr<HttpBackground> background,
            boost::beast::tcp_stream&& stream,
            boost::beast::flat_buffer&& buffer,
            boost::beast::http::request<
                boost::beast::http::string_body>&& request,
            bool is_authorized,
            Completion completion);
        ~EndpointSession() override;

        EndpointSession(const EndpointSession&) = delete;
        EndpointSession& operator=(const EndpointSession&) = delete;

        // run() and close() are safe to call from any thread. run() may only
        // open the gateway once; later calls are ignored.
        void run();
        void close();

        // Reserves queue entries and bytes before returning. The package
        // bridge treats false as terminal backpressure.
        bool emit(EndpointOutput output) override;

    private:
        class State;
        std::shared_ptr<State> state_;
    };

} // namespace circud
