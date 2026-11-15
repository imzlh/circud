#pragma once

#include <circud/package_runtime.hpp>

#include <quickjs.h>

#include <functional>
#include <string>

namespace circud::host {

    struct BindingConfig {
        std::string package_id;
        std::string package_dir;
        std::string data_dir;
        OutboundCall outbound;
        std::function<void(std::function<void(JSContext*)>)> post;
        std::function<void()> request_reload;
        std::size_t max_endpoint_response_bytes = kMaxEndpointEnvelopeBytes;
        std::size_t max_endpoint_message_bytes = kMaxEndpointEnvelopeBytes;
    };

    void register_runtime(TJSRuntime* runtime, BindingConfig config);
    void unregister_runtime(TJSRuntime* runtime);
    void shutdown_runtime(JSContext* ctx);
    void dispatch_call(JSContext* ctx, RuntimeCall call);
    void dispatch_cancel(JSContext* ctx, std::uint64_t id);
    void dispatch_endpoint(JSContext* ctx, EndpointRequest request,
        std::shared_ptr<EndpointSink> sink);
    void dispatch_endpoint_message(JSContext* ctx, EndpointMessage message);
    void dispatch_endpoint_close(JSContext* ctx, EndpointClose close);

} // namespace circud::host

extern "C" void circud_module_init(JSContext* ctx, JSValue ns);
