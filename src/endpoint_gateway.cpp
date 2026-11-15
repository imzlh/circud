#include <circud/endpoint_gateway.hpp>

#include <circud/core.hpp>
#include <circud/manifest.hpp>
#include <circud/package_runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace circud {
    namespace {

        bool contains_scope(std::string_view path,
            std::string_view prefix) noexcept {
            return path == prefix ||
                (path.size() > prefix.size() && path.starts_with(prefix) &&
                    path[prefix.size()] == '/');
        }

        bool terminal_output(const EndpointOutput& output) noexcept {
            return std::holds_alternative<EndpointResponse>(output) ||
                std::holds_alternative<EndpointStatic>(output) ||
                std::holds_alternative<EndpointClose>(output);
        }

        std::uint64_t output_id(const EndpointOutput& output) noexcept {
            return std::visit([](const auto& value) { return value.id; }, output);
        }

        std::size_t request_size(const EndpointRequest& request) noexcept {
            std::size_t bytes = request.method.size() + request.path.size() +
                request.query.size() + request.remote_address.size() +
                0;
            const auto add = [&bytes](const std::size_t amount) {
                if (amount > kMaxEndpointEnvelopeBytes -
                        std::min(bytes, kMaxEndpointEnvelopeBytes)) {
                    bytes = kMaxEndpointEnvelopeBytes + 1;
                }
                else {
                    bytes += amount;
                }
            };
            for (const auto& [name, value] : request.headers) {
                add(name.size());
                add(value.size());
            }
            for (const auto& protocol : request.offered_subprotocols)
                add(protocol.size());
            return bytes;
        }

    } // namespace

    struct EndpointGateway::State {
        struct Route {
            std::string prefix;
            std::string package_id;
        };

        struct Session {
            Session(std::string package_id,
                std::weak_ptr<PackageRuntime> runtime,
                std::shared_ptr<EndpointSink> downstream)
                : package_id(std::move(package_id)),
                  runtime(std::move(runtime)),
                  downstream(std::move(downstream)) {}

            bool emit(EndpointOutput output) noexcept {
                std::lock_guard lock(mutex);
                if (!active) return false;
                const bool terminal = terminal_output(output);
                bool accepted = false;
                try {
                    accepted = downstream->emit(std::move(output));
                }
                catch (...) {
                    accepted = false;
                }
                if (terminal || !accepted) {
                    active = false;
                    downstream.reset();
                }
                return accepted;
            }

            void deactivate() noexcept {
                try {
                    std::lock_guard lock(mutex);
                    active = false;
                    downstream.reset();
                }
                catch (...) {}
            }

            void invalidate(std::uint64_t id) noexcept {
                try {
                    std::lock_guard lock(mutex);
                    if (!active) return;
                    active = false;
                    EndpointClose close;
                    close.id = id;
                    close.code = 1001;
                    close.reason = "package runtime stopped";
                    try {
                        (void)downstream->emit(std::move(close));
                    }
                    catch (...) {}
                    downstream.reset();
                }
                catch (...) {}
            }

            std::string package_id;
            std::weak_ptr<PackageRuntime> runtime;
            std::shared_ptr<EndpointSink> downstream;
            std::mutex mutex;
            bool active = true;
        };

        explicit State(EndpointConfig limits) : limits(std::move(limits)) {}

        bool contains(std::uint64_t id,
            const std::shared_ptr<Session>& expected) const {
            std::lock_guard lock(mutex);
            const auto found = sessions.find(id);
            return found != sessions.end() && found->second == expected;
        }

        void release(std::uint64_t id,
            const std::shared_ptr<Session>& expected = {}) noexcept {
            auto released = extract(id, expected);
            if (released) released->deactivate();
        }

        std::shared_ptr<Session> extract(std::uint64_t id,
            const std::shared_ptr<Session>& expected = {}) noexcept {
            {
                std::lock_guard lock(mutex);
                const auto found = sessions.find(id);
                if (found == sessions.end() ||
                    (expected && found->second != expected))
                    return {};
                auto released = found->second;
                const auto count = per_package.find(released->package_id);
                if (count != per_package.end()) {
                    if (count->second > 1) --count->second;
                    else per_package.erase(count);
                }
                sessions.erase(found);
                return released;
            }
        }

        mutable std::mutex mutex;
        EndpointConfig limits;
        std::vector<Route> routes;
        std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
        std::unordered_map<std::string, std::size_t> per_package;
        std::atomic<std::uint64_t> next_id{ 1 };
        bool stopping = false;
    };

    class EndpointGateway::GatewaySink final : public EndpointSink {
    public:
        GatewaySink(std::weak_ptr<State> state,
            std::shared_ptr<State::Session> session, std::uint64_t id)
            : state_(std::move(state)), session_(std::move(session)), id_(id) {}

        bool emit(EndpointOutput output) override {
            const auto state = state_.lock();
            if (!state) return false;
            if (output_id(output) != id_) {
                state->release(id_, session_);
                return false;
            }
            if (!state->contains(id_, session_)) return false;
            const bool terminal = terminal_output(output);
            const bool accepted = session_->emit(std::move(output));
            if (terminal || !accepted) state->release(id_, session_);
            return accepted;
        }

    private:
        std::weak_ptr<State> state_;
        std::shared_ptr<State::Session> session_;
        std::uint64_t id_ = 0;
    };

    EndpointGateway::EndpointGateway(Core& core)
        : core_(core), state_(std::make_shared<State>(core.config().endpoints)) {}

    EndpointGateway::~EndpointGateway() {
        shutdown();
    }

    void EndpointGateway::validate_scopes(
        const std::vector<PackageManifest>& manifests) {
        struct Scope {
            std::string prefix;
            std::string package_id;
        };
        std::vector<Scope> scopes;
        scopes.reserve(manifests.size());
        for (const auto& manifest : manifests) {
            scopes.push_back(
                { effective_endpoint_prefix(manifest), manifest.id });
        }
        for (std::size_t left = 0; left < scopes.size(); ++left) {
            for (std::size_t right = left + 1; right < scopes.size(); ++right) {
                if (!contains_scope(scopes[left].prefix, scopes[right].prefix) &&
                    !contains_scope(scopes[right].prefix, scopes[left].prefix))
                    continue;
                throw std::runtime_error("endpoint prefix for " +
                    scopes[left].package_id + " overlaps package " +
                    scopes[right].package_id);
            }
        }
    }

    void EndpointGateway::publish(const std::vector<StoredPackage>& packages) {
        std::vector<PackageManifest> manifests;
        manifests.reserve(packages.size());
        for (const auto& package : packages)
            manifests.push_back(package.manifest);
        validate_scopes(manifests);

        std::vector<State::Route> routes;
        routes.reserve(packages.size());
        for (const auto& package : packages) {
            routes.push_back({ effective_endpoint_prefix(package.manifest),
                package.manifest.id });
        }
        std::sort(routes.begin(), routes.end(), [](const auto& left,
                                                   const auto& right) {
            return left.prefix.size() > right.prefix.size();
        });
        std::lock_guard lock(state_->mutex);
        state_->routes = std::move(routes);
        state_->stopping = false;
    }

    EndpointOpenResult EndpointGateway::open(EndpointRequest request,
        std::shared_ptr<EndpointSink> sink) {
        const auto fail = [](EndpointOpenStatus status, std::string message) {
            EndpointOpenResult result;
            result.status = status;
            result.message = std::move(message);
            return result;
        };
        if (!sink || request.id != 0 || request.method.empty() ||
            request.path.empty() || request.path.front() != '/' ||
            request.path.find_first_of("?#\\") != std::string::npos ||
            request.body.size() > state_->limits.max_request_body_bytes ||
            request_size(request) > kMaxEndpointEnvelopeBytes ||
            (request.protocol != EndpointProtocol::http &&
                request.protocol != EndpointProtocol::websocket)) {
            return fail(EndpointOpenStatus::invalid_request,
                "invalid endpoint request");
        }
        if (!state_->limits.enabled)
            return fail(EndpointOpenStatus::disabled,
                "package endpoints are disabled");

        State::Route route;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopping)
                return fail(EndpointOpenStatus::unavailable,
                    "endpoint gateway is stopping");
            const auto found = std::find_if(state_->routes.begin(),
                state_->routes.end(), [&](const auto& candidate) {
                    return contains_scope(request.path, candidate.prefix);
                });
            if (found == state_->routes.end())
                return fail(EndpointOpenStatus::not_found,
                    "no package owns this endpoint scope");
            route = *found;
        }

        const auto id = state_->next_id.fetch_add(1);
        if (id == 0)
            return fail(EndpointOpenStatus::overloaded,
                "endpoint session id space exhausted");

        std::shared_ptr<PackageRuntime> runtime;
        std::shared_ptr<State::Session> session;
        {
            // Runtime retirement removes the current generation under the
            // core mutex before release_runtime() scans reservations. Publish
            // the reservation while that same mutex is held so retirement
            // cannot miss an endpoint admitted against the old generation.
            std::lock_guard core_lock(core_.mutex_);
            if (!core_.started_)
                return fail(EndpointOpenStatus::unavailable,
                    "circud is not running");
            const auto found = core_.runtimes_.find(route.package_id);
            if (found == core_.runtimes_.end())
                return fail(EndpointOpenStatus::unavailable,
                    "package runtime is not running");
            runtime = found->second;
            if (effective_endpoint_prefix(runtime->package().manifest) !=
                route.prefix) {
                return fail(EndpointOpenStatus::unavailable,
                    "endpoint route does not match the current package runtime");
            }
            const auto runtime_state = runtime->state();
            if (runtime_state != RuntimeState::running &&
                runtime_state != RuntimeState::stuck)
                return fail(EndpointOpenStatus::unavailable,
                    "package runtime is unavailable");

            session = std::make_shared<State::Session>(
                route.package_id, runtime, sink);
            std::lock_guard state_lock(state_->mutex);
            auto& package_count = state_->per_package[route.package_id];
            if (state_->stopping ||
                state_->sessions.size() >= state_->limits.max_sessions ||
                package_count >= state_->limits.max_sessions_per_package) {
                if (package_count == 0)
                    state_->per_package.erase(route.package_id);
                return fail(EndpointOpenStatus::overloaded,
                    "endpoint session limit reached");
            }
            ++package_count;
            state_->sessions.emplace(id, session);
        }

        request.id = id;
        request.path = request.path == route.prefix
            ? std::string("/")
            : request.path.substr(route.prefix.size());
        if (request.deadline == std::chrono::steady_clock::time_point{})
            request.deadline = std::chrono::steady_clock::now() +
                state_->limits.idle_timeout;
        auto gateway_sink = std::make_shared<GatewaySink>(state_, session, id);
        if (!runtime->open_endpoint(std::move(request),
                std::move(gateway_sink))) {
            state_->release(id);
            return fail(EndpointOpenStatus::overloaded,
                "package endpoint queue is unavailable");
        }

        EndpointOpenResult result;
        result.status = EndpointOpenStatus::accepted;
        result.session_id = id;
        result.package_id = std::move(route.package_id);
        result.package_version_dir = runtime->package().version_dir;
        return result;
    }

    bool EndpointGateway::owns_scope(std::string_view path) const noexcept {
        if (path.empty() || path.front() != '/' ||
            path.find_first_of("?#\\") != std::string_view::npos)
            return false;
        std::lock_guard lock(state_->mutex);
        return std::ranges::any_of(state_->routes,
            [path](const auto& route) {
                return contains_scope(path, route.prefix);
            });
    }

    bool EndpointGateway::message(std::uint64_t session_id,
        EndpointBytes data, bool binary) {
        if (session_id == 0 || data.size() > state_->limits.max_message_bytes)
            return false;
        std::shared_ptr<PackageRuntime> runtime;
        std::string package_id;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->sessions.find(session_id);
            if (found == state_->sessions.end()) return false;
            runtime = found->second->runtime.lock();
            package_id = found->second->package_id;
        }
        if (!runtime) {
            state_->release(session_id);
            return false;
        }
        {
            std::lock_guard lock(core_.mutex_);
            const auto current = core_.runtimes_.find(package_id);
            if (!core_.started_ || current == core_.runtimes_.end() ||
                current->second.get() != runtime.get()) {
                state_->release(session_id);
                return false;
            }
        }
        EndpointMessage message;
        message.id = session_id;
        message.data = std::move(data);
        message.binary = binary;
        if (runtime->deliver_endpoint_message(std::move(message))) return true;

        // The network session treats inbound backpressure as terminal and does
        // not issue a second gateway close. Queue its close on the independent
        // control path here, then release only after reliable acceptance.
        EndpointClose close;
        close.id = session_id;
        close.code = 1013;
        close.reason = "package endpoint queue is unavailable";
        if (runtime->close_endpoint(std::move(close))) {
            state_->release(session_id);
            return false;
        }

        // A live generation keeps its reservation if even the control command
        // was not accepted. This is not expected for a valid session because
        // the close queue covers the per-package session bound plus admitted
        // opens that can reuse reservations from older accepted closes.
        bool runtime_disappeared = false;
        {
            std::lock_guard lock(core_.mutex_);
            const auto current = core_.runtimes_.find(package_id);
            const auto runtime_state = runtime->state();
            runtime_disappeared = !core_.started_ ||
                current == core_.runtimes_.end() ||
                current->second.get() != runtime.get() ||
                (runtime_state != RuntimeState::running &&
                    runtime_state != RuntimeState::stuck);
        }
        if (runtime_disappeared) state_->release(session_id);
        return false;
    }

    bool EndpointGateway::close(std::uint64_t session_id, std::uint16_t code,
        std::string reason) {
        if (session_id == 0 || code < 1000 || code > 4999 ||
            reason.size() > 123)
            return false;
        std::shared_ptr<PackageRuntime> runtime;
        std::string package_id;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->sessions.find(session_id);
            if (found == state_->sessions.end()) return false;
            runtime = found->second->runtime.lock();
            package_id = found->second->package_id;
        }
        if (!runtime) {
            state_->release(session_id);
            return false;
        }
        EndpointClose command;
        command.id = session_id;
        command.code = code;
        command.reason = std::move(reason);
        if (runtime->close_endpoint(std::move(command))) {
            state_->release(session_id);
            return true;
        }

        // A live generation keeps its gateway reservation when a control
        // command was not accepted. This permits a later retry without
        // claiming that the package observed the close. A stopped/replaced
        // generation has nowhere left to deliver it, so release it here.
        bool runtime_disappeared = false;
        {
            std::lock_guard lock(core_.mutex_);
            const auto current = core_.runtimes_.find(package_id);
            const auto runtime_state = runtime->state();
            runtime_disappeared = !core_.started_ ||
                current == core_.runtimes_.end() ||
                current->second.get() != runtime.get() ||
                (runtime_state != RuntimeState::running &&
                    runtime_state != RuntimeState::stuck);
        }
        if (runtime_disappeared) state_->release(session_id);
        return false;
    }

    std::size_t EndpointGateway::active_sessions() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->sessions.size();
    }

    void EndpointGateway::release_runtime(
        const std::shared_ptr<PackageRuntime>& runtime) {
        if (!runtime) return;
        std::vector<std::pair<std::uint64_t,
            std::shared_ptr<State::Session>>> sessions;
        {
            std::lock_guard lock(state_->mutex);
            sessions.reserve(state_->sessions.size());
            for (const auto& [id, session] : state_->sessions) {
                if (!session->runtime.owner_before(runtime) &&
                    !runtime.owner_before(session->runtime))
                    sessions.emplace_back(id, session);
            }
        }
        for (const auto& [id, session] : sessions) {
            const auto released = state_->extract(id, session);
            if (released) released->invalidate(id);
        }
    }

    void EndpointGateway::reset() noexcept {
        if (!state_) return;
        decltype(state_->sessions) sessions;
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
            sessions.swap(state_->sessions);
            state_->per_package.clear();
            state_->routes.clear();
        }
        for (const auto& [id, session] : sessions)
            session->invalidate(id);
    }

    void EndpointGateway::shutdown() noexcept {
        if (!state_) return;
        decltype(state_->sessions) sessions;
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
            sessions.swap(state_->sessions);
            state_->per_package.clear();
        }
        for (const auto& [id, session] : sessions)
            session->invalidate(id);
    }

    std::string_view to_string(EndpointOpenStatus status) noexcept {
        switch (status) {
        case EndpointOpenStatus::accepted: return "accepted";
        case EndpointOpenStatus::disabled: return "disabled";
        case EndpointOpenStatus::invalid_request: return "invalid_request";
        case EndpointOpenStatus::not_found: return "not_found";
        case EndpointOpenStatus::unavailable: return "unavailable";
        case EndpointOpenStatus::overloaded: return "overloaded";
        }
        return "unknown";
    }

} // namespace circud
