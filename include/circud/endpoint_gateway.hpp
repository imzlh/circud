#pragma once

#include <circud/database.hpp>
#include <circud/package_endpoint.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace circud {

    class Core;
    class PackageRuntime;

    enum class EndpointOpenStatus {
        accepted,
        disabled,
        invalid_request,
        not_found,
        unavailable,
        overloaded,
    };

    struct EndpointOpenResult {
        EndpointOpenStatus status = EndpointOpenStatus::unavailable;
        std::uint64_t session_id = 0;
        std::string package_id;
        std::filesystem::path package_version_dir;
        std::string message;

        explicit operator bool() const noexcept {
            return status == EndpointOpenStatus::accepted;
        }
    };

    // Resolves package-owned URL scopes and binds each accepted connection to
    // one exact PackageRuntime generation. Protocol-specific socket state stays
    // in the network layer; this class only owns routing, quotas and handoff.
    class EndpointGateway {
    public:
        explicit EndpointGateway(Core& core);
        ~EndpointGateway();
        EndpointGateway(const EndpointGateway&) = delete;
        EndpointGateway& operator=(const EndpointGateway&) = delete;

        EndpointOpenResult open(EndpointRequest request,
            std::shared_ptr<EndpointSink> sink);
        bool owns_scope(std::string_view path) const noexcept;
        bool message(std::uint64_t session_id, EndpointBytes data,
            bool binary = false);
        bool close(std::uint64_t session_id, std::uint16_t code = 1000,
            std::string reason = {});

        // Publishes a complete immutable-by-replacement scope table. It throws
        // before publication if two package prefixes are equal or nested.
        void publish(const std::vector<StoredPackage>& packages);
        static void validate_scopes(
            const std::vector<PackageManifest>& manifests);

        std::size_t active_sessions() const noexcept;
        void release_runtime(const std::shared_ptr<PackageRuntime>& runtime);
        void reset() noexcept;
        void shutdown() noexcept;

    private:
        struct State;
        class GatewaySink;
        Core& core_;
        std::shared_ptr<State> state_;
    };

    std::string_view to_string(EndpointOpenStatus status) noexcept;

} // namespace circud
