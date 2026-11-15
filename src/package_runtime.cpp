#include <circud/package_runtime.hpp>

#include "host_module.hpp"
#include "message_clone.h"
#include "tjs.h"

#include <uv.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace circud {
    namespace {

        bool path_within(const std::filesystem::path& path,
            const std::filesystem::path& root) {
            std::error_code error;
            auto normalized_path = std::filesystem::weakly_canonical(path, error);
            if (error) normalized_path = path.lexically_normal();
            error.clear();
            auto normalized_root = std::filesystem::weakly_canonical(root, error);
            if (error) normalized_root = root.lexically_normal();
            auto mismatch = std::mismatch(normalized_root.begin(), normalized_root.end(),
                normalized_path.begin(), normalized_path.end());
            return mismatch.first == normalized_root.end();
        }

        bool matches_name(const std::vector<std::string>& values,
            std::string_view value) {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        std::size_t endpoint_header_bytes(const EndpointHeaders& headers) {
            std::size_t total = 0;
            for (const auto& [name, value] : headers) {
                if (name.size() > kMaxEndpointEnvelopeBytes - total)
                    return kMaxEndpointEnvelopeBytes + 1;
                total += name.size();
                if (value.size() > kMaxEndpointEnvelopeBytes - total)
                    return kMaxEndpointEnvelopeBytes + 1;
                total += value.size();
            }
            return total;
        }

        std::size_t endpoint_close_capacity(
            const EndpointConfig& config) noexcept {
            const auto active =
                std::max<std::size_t>(1, config.max_sessions_per_package);
            if (active > std::numeric_limits<std::size_t>::max() -
                    kMaxEndpointCommands)
                return std::numeric_limits<std::size_t>::max();
            // Accepted closes release gateway reservations immediately. While
            // the owner thread is busy, those reservations may be reused by
            // opens already admitted to the bounded data-command queue.
            return active + kMaxEndpointCommands;
        }

        std::size_t endpoint_pending_close_capacity(
            const EndpointConfig& config) noexcept {
            const auto queued = endpoint_close_capacity(config);
            if (queued > std::numeric_limits<std::size_t>::max() / 2)
                return std::numeric_limits<std::size_t>::max();
            // drain() swaps one batch to owner-thread-local storage. Keep one
            // equally bounded producer batch available while it is dispatching.
            return queued * 2;
        }

        EndpointClose stopped_endpoint(std::uint64_t id,
            std::string message) {
            return EndpointClose{ id, 1001, std::move(message) };
        }

        void emit_endpoint_noexcept(const std::shared_ptr<EndpointSink>& sink,
            EndpointOutput output) noexcept {
            if (!sink) return;
            try {
                (void)sink->emit(std::move(output));
            }
            catch (...) {
                // A network sink is required to be non-throwing in practice;
                // never let a faulty sink unwind through the runtime thread.
            }
        }

        void complete_call_noexcept(RuntimeCall& call,
            CallResult result) noexcept {
            auto completion = std::move(call.complete);
            if (!completion) return;
            try {
                completion(std::move(result));
            }
            catch (...) {
                // Completions cross back to the control actor. A faulty
                // consumer must not unwind through the runtime owner thread.
            }
        }

        void fail_call_noexcept(RuntimeCall& call, CallErrorCode code,
            std::string_view message) noexcept {
            try {
                complete_call_noexcept(call,
                    CallResult::failure(code, std::string(message)));
            }
            catch (...) {
                // An allocation failure cannot be represented as CallResult.
                // Core still owns the authoritative pending call and timeout.
                call.complete = {};
            }
        }

        class RuntimeStopGuard {
        public:
            explicit RuntimeStopGuard(TJSRuntime* runtime) noexcept
                : runtime_(runtime) {}
            ~RuntimeStopGuard() {
                if (runtime_) TJS_Stop(runtime_);
            }
            RuntimeStopGuard(const RuntimeStopGuard&) = delete;
            RuntimeStopGuard& operator=(const RuntimeStopGuard&) = delete;

        private:
            TJSRuntime* runtime_;
        };

    } // namespace

    PackageRuntime::PackageRuntime(StoredPackage package, OutboundCall outbound,
        std::function<void()> request_reload,
        EndpointConfig endpoint_config)
        : package_(std::move(package)), outbound_(std::move(outbound)),
        request_reload_(std::move(request_reload)),
          endpoint_config_(std::move(endpoint_config)),
          max_endpoint_command_bytes_(std::max({ kMaxEndpointCommandBytes,
              endpoint_config_.max_request_body_bytes +
                  kMaxEndpointEnvelopeBytes,
              endpoint_config_.max_message_bytes })),
          max_endpoint_closes_(endpoint_close_capacity(endpoint_config_)),
          max_pending_endpoint_closes_(
              endpoint_pending_close_capacity(endpoint_config_)) {
        // Lifecycle notifications are admitted from control threads but run
        // on the package owner thread. Reserve their complete bounded storage
        // up front, so exit teardown itself cannot fail while publishing them.
        graceful_stop_waiters_.reserve(max_lifecycle_waiters_);
        exit_waiters_.reserve(max_lifecycle_waiters_);
    }

    PackageRuntime::~PackageRuntime() {
        // Normal running ownership reaches the destructor only after Core's
        // reaper has joined the owner thread. Keep a conservative fallback for
        // construction failures and standalone users.
        if (!thread_.joinable()) return;
        if (thread_.get_id() == std::this_thread::get_id()) {
            // The thread lambda deliberately owns one reference until run()
            // returns. If no external owner remains, reclamation is already on
            // the finished owner thread and there is nothing left to join.
            thread_.detach();
            return;
        }
        stop_and_join();
    }

    void PackageRuntime::async_start(StartCompletion completion) {
        {
            std::lock_guard lock(mutex_);
            if (thread_.joinable())
                throw std::logic_error("package runtime already started");
            state_.store(RuntimeState::starting);
            join_claimed_.store(false);
            exited_.store(false);
            stopping_ = false;
            graceful_stop_started_ = false;
            graceful_stop_finished_ = false;
            graceful_stop_waiters_.clear();
            exit_waiters_.clear();
            failure_.clear();
        }
        std::shared_ptr<StartCompletion> start;
        try {
            // Allocate first, then move the callback with noexcept semantics;
            // allocation or std::thread construction failure can still notify
            // the original caller exactly once.
            start = std::make_shared<StartCompletion>();
            *start = std::move(completion);
            auto self = shared_from_this();
            thread_ = std::thread(
                [self = std::move(self), start]() mutable {
                    self->run(std::move(*start));
                });
        }
        catch (const std::exception& exception) {
            fail(exception.what());
            auto& failed = start ? *start : completion;
            if (failed) {
                try { failed(RuntimeState::failed); }
                catch (...) {}
            }
            signal_exited();
        }
        catch (...) {
            fail("cannot create package owner thread");
            auto& failed = start ? *start : completion;
            if (failed) {
                try { failed(RuntimeState::failed); }
                catch (...) {}
            }
            signal_exited();
        }
    }

    bool PackageRuntime::async_graceful_stop(std::string reason,
        StopCompletion completion) {
        bool enqueue_stop = false;
        bool complete_now = false;
        {
            std::lock_guard lock(mutex_);
            if (!thread_.joinable()) {
                complete_now = true;
            }
            else {
                const auto state = state_.load();
                if (graceful_stop_finished_ || stopping_ ||
                    (state != RuntimeState::running &&
                    state != RuntimeState::stuck)) {
                    complete_now = true;
                }
                else {
                    if (completion) {
                        if (graceful_stop_waiters_.size() >=
                            max_lifecycle_waiters_)
                            return false;
                        // Capacity was reserved by the constructor.
                        graceful_stop_waiters_.push_back(
                            std::move(completion));
                    }
                    if (!graceful_stop_started_) {
                        graceful_stop_started_ = true;
                        enqueue_stop = true;
                    }
                }
            }
        }
        if (complete_now) {
            if (completion) {
                try { completion(); }
                catch (...) {}
            }
            return true;
        }
        if (!enqueue_stop) return true;

        /* Give the package one bounded opportunity to terminate and wait for its
         * own children. Core owns the timeout; this function never waits. */
        RuntimeCall call;
        call.id = std::numeric_limits<std::uint64_t>::max();
        call.source = "core";
        call.target = "lifecycle.stop";
        call.json_arguments = "{\"reason\":\"" + reason + "\"}";
        call.deadline =
            std::chrono::steady_clock::now() + package_.manifest.shutdown_timeout;
        std::weak_ptr<PackageRuntime> weak = weak_from_this();
        call.complete = [weak](CallResult) {
            if (auto self = weak.lock()) self->finish_graceful_stop();
        };
        if (!enqueue(std::move(call))) {
            finish_graceful_stop();
        }
        return true;
    }

    void PackageRuntime::request_shutdown(std::string reason) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!thread_.joinable() || stopping_) return;
            stopping_ = true;
            if (failure_.empty() && reason != "shutdown") failure_ = std::move(reason);
            if (state_.load() != RuntimeState::failed)
                state_.store(RuntimeState::stopping);
            if (async_) uv_async_send(reinterpret_cast<uv_async_t*>(async_));
        }
    }

    bool PackageRuntime::notify_on_exit(std::function<void()> completion) {
        if (!completion) return true;
        bool complete_now = false;
        {
            std::lock_guard lock(mutex_);
            if (exited_.load()) complete_now = true;
            else {
                if (exit_waiters_.size() >= max_lifecycle_waiters_)
                    return false;
                // Capacity was reserved by the constructor.
                exit_waiters_.push_back(std::move(completion));
            }
        }
        if (complete_now) {
            try { completion(); }
            catch (...) {}
        }
        return true;
    }

    bool PackageRuntime::claim_join() noexcept {
        bool expected = false;
        return join_claimed_.compare_exchange_strong(expected, true);
    }

    void PackageRuntime::join() {
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id())
                throw std::logic_error("package runtime cannot join itself");
            thread_.join();
        }
        if (state_.load() != RuntimeState::failed)
            state_.store(RuntimeState::disabled);
    }

    void PackageRuntime::stop_and_join(std::string reason) {
        request_shutdown(std::move(reason));
        if (claim_join()) join();
    }

    bool PackageRuntime::enqueue(RuntimeCall call) {
        const auto bytes = call.arguments
            ? tjs__message_clone_size(call.arguments.get())
            : call.json_arguments ? call.json_arguments->size() : 0;
        std::lock_guard lock(mutex_);
        const auto state = state_.load();
        if ((state != RuntimeState::running && state != RuntimeState::stuck) ||
            stopping_ || !async_)
            return false;
        if (queue_.size() >= kMaxQueueEntries ||
            bytes > kMaxEnvelopeBytes || queue_bytes_ + bytes > kMaxQueueBytes)
            return false;
        queue_bytes_ += bytes;
        queue_.push_back(std::move(call));
        uv_async_send(reinterpret_cast<uv_async_t*>(async_));
        return true;
    }

    void PackageRuntime::mark_stuck() {
        RuntimeState expected = RuntimeState::running;
        state_.compare_exchange_strong(expected, RuntimeState::stuck);
    }

    void PackageRuntime::clear_stuck() {
        RuntimeState expected = RuntimeState::stuck;
        state_.compare_exchange_strong(expected, RuntimeState::running);
    }

    void PackageRuntime::cancel(std::uint64_t id) {
        if (id == 0) return;
        std::lock_guard lock(mutex_);
        const auto state = state_.load();
        if ((state != RuntimeState::running && state != RuntimeState::stuck) ||
            stopping_ || !async_ ||
            pending_call_cancellation_ids_.contains(id) ||
            pending_call_cancellation_ids_.size() >= max_call_cancellations_)
            return;
        try {
            pending_call_cancellation_ids_.insert(id);
            try {
                call_cancellations_.push_back(id);
            }
            catch (...) {
                pending_call_cancellation_ids_.erase(id);
                return;
            }
        }
        catch (...) {
            return;
        }
        uv_async_send(reinterpret_cast<uv_async_t*>(async_));
    }

    bool PackageRuntime::open_endpoint(EndpointRequest request,
        std::shared_ptr<EndpointSink> sink) {
        if (!sink || request.id == 0) return false;
        std::size_t metadata_bytes =
            request.method.size() + request.path.size() + request.query.size() +
            request.remote_address.size() +
            endpoint_header_bytes(request.headers);
        for (const auto& protocol : request.offered_subprotocols) {
            if (protocol.size() > kMaxEndpointEnvelopeBytes -
                    std::min(metadata_bytes, kMaxEndpointEnvelopeBytes)) {
                metadata_bytes = kMaxEndpointEnvelopeBytes + 1;
                break;
            }
            metadata_bytes += protocol.size();
        }
        if (metadata_bytes > kMaxEndpointEnvelopeBytes ||
            request.body.size() > endpoint_config_.max_request_body_bytes ||
            request.body.size() > max_endpoint_command_bytes_ -
                std::min(metadata_bytes, max_endpoint_command_bytes_))
            return false;
        const auto bytes = metadata_bytes + request.body.size();
        std::lock_guard lock(mutex_);
        const auto state = state_.load();
        if ((state != RuntimeState::running && state != RuntimeState::stuck) ||
            stopping_ || !async_)
            return false;
        if (endpoint_commands_.size() >= kMaxEndpointCommands ||
            bytes > max_endpoint_command_bytes_ ||
            endpoint_command_bytes_ > max_endpoint_command_bytes_ - bytes)
            return false;
        endpoint_command_bytes_ += bytes;
        endpoint_commands_.push_back(OpenEndpoint{
            std::move(request), std::move(sink) });
        uv_async_send(reinterpret_cast<uv_async_t*>(async_));
        return true;
    }

    bool PackageRuntime::deliver_endpoint_message(EndpointMessage message) {
        if (message.id == 0 ||
            message.data.size() > endpoint_config_.max_message_bytes)
            return false;
        const auto bytes = message.data.size();
        EndpointCommand command = std::move(message);
        std::lock_guard lock(mutex_);
        const auto state = state_.load();
        if ((state != RuntimeState::running && state != RuntimeState::stuck) ||
            stopping_ || !async_)
            return false;
        if (endpoint_commands_.size() >= kMaxEndpointCommands ||
            bytes > max_endpoint_command_bytes_ ||
            endpoint_command_bytes_ > max_endpoint_command_bytes_ - bytes)
            return false;
        endpoint_command_bytes_ += bytes;
        endpoint_commands_.push_back(std::move(command));
        uv_async_send(reinterpret_cast<uv_async_t*>(async_));
        return true;
    }

    bool PackageRuntime::close_endpoint(EndpointClose close) {
        if (close.id == 0 || close.code < 1000 || close.code > 4999 ||
            close.reason.size() > 123)
            return false;
        std::lock_guard lock(mutex_);
        const auto state = state_.load();
        if ((state != RuntimeState::running && state != RuntimeState::stuck) ||
            stopping_ || !async_)
            return false;
        if (pending_endpoint_close_ids_.contains(close.id)) return true;
        if (endpoint_closes_.size() >= max_endpoint_closes_ ||
            pending_endpoint_close_ids_.size() >=
                max_pending_endpoint_closes_)
            return false;
        const auto id = close.id;
        try {
            pending_endpoint_close_ids_.insert(id);
            try {
                endpoint_closes_.push_back(std::move(close));
            }
            catch (...) {
                pending_endpoint_close_ids_.erase(id);
                return false;
            }
        }
        catch (...) {
            return false;
        }
        uv_async_send(reinterpret_cast<uv_async_t*>(async_));
        return true;
    }

    bool PackageRuntime::post(std::function<void(JSContext*)> task) {
        std::lock_guard lock(mutex_);
        if (stopping_ || !async_ || tasks_.size() >= max_tasks_) return false;
        try {
            tasks_.push_back(std::move(task));
        }
        catch (...) {
            return false;
        }
        uv_async_send(reinterpret_cast<uv_async_t*>(async_));
        return true;
    }

    std::string PackageRuntime::failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    void PackageRuntime::async_callback(uv_async_s* handle) noexcept {
        auto* self = static_cast<PackageRuntime*>(handle->data);
        try {
            self->drain();
        }
        catch (const std::exception& exception) {
            self->fail(exception.what());
            self->request_shutdown("runtime command dispatch failed");
            if (self->runtime_) TJS_Stop(self->runtime_);
        }
        catch (...) {
            self->fail("runtime command dispatch failed");
            self->request_shutdown("runtime command dispatch failed");
            if (self->runtime_) TJS_Stop(self->runtime_);
        }
    }

    bool PackageRuntime::allowed_path(std::string_view resource, bool write) const {
        if (resource.empty()) return false;
        const std::filesystem::path path(resource);
        if (path_within(path, package_.data_dir)) return true;
        if (!write && path_within(path, package_.version_dir)) return true;
        const auto& extra = write ? package_.manifest.permissions.write
            : package_.manifest.permissions.read;
        for (const auto& root : extra)
            if (path_within(path, root)) return true;
        return false;
    }

    void PackageRuntime::run(StartCompletion completion) noexcept {
        const auto ready = [this, &completion]() noexcept {
            if (!completion) return;
            try {
                completion(state_.load());
            }
            catch (...) {
                // Lifecycle callbacks are notification-only and must not unwind
                // through the package owner thread.
            }
            completion = {};
        };
        try {
            runtime_ = TJS_NewRuntimeWorker();
            if (!runtime_)
                throw std::runtime_error("cannot create libcjs runtime");

            context_ = TJS_GetMainContext(runtime_);
            TJS_SetCapabilityHook(
                runtime_,
                [](JSContext* ctx, const TJSCapability* capability,
                    void* userdata) -> JSValue {
                        auto* self = static_cast<PackageRuntime*>(userdata);
                        const auto category =
                            TJS_CAP_CATEGORY(capability->mask);
                        const auto operations =
                            TJS_CAP_OPERATIONS(capability->mask);
                        bool allowed = false;
                        switch (category) {
                        case TJS_CAP_IO: {
                            const bool write =
                                (operations & (TJS_CAP_IO_WRITE |
                                    TJS_CAP_IO_CONTROL)) != 0;
                            const char* resource = capability->details.resource;
                            allowed = resource &&
                                self->allowed_path(resource, write);
                            break;
                        }
                        case TJS_CAP_ENV:
                            allowed = capability->details.resource &&
                                matches_name(
                                    self->package_.manifest.permissions.env,
                                    capability->details.resource);
                            break;
                        case TJS_CAP_NETWORK:
                            allowed = self->package_.manifest.permissions.net;
                            break;
                        case TJS_CAP_PROCESS:
                            allowed = self->package_.manifest.permissions.run;
                            break;
                        case TJS_CAP_DYNLIB:
                            allowed = self->package_.manifest.permissions.ffi;
                            break;
                        case TJS_CAP_CONSOLE:
                            allowed = true;
                            break;
                        case TJS_CAP_OS:
                        case TJS_CAP_POSIX:
                        case TJS_CAP_SHM:
                        case TJS_CAP_DEBUG:
                        case TJS_CAP_WORKER:
                            allowed = self->package_.manifest.permissions.sys;
                            break;
                        default:
                            allowed = false;
                        }
                        if (allowed) return TJS_CAPABILITY_PASS;
                        return JS_ThrowTypeError(ctx,
                            "capability denied: %s", capability->api);
                },
                this, 0);

            auto async = std::make_unique<uv_async_t>();
            async->data = this;
            if (uv_async_init(TJS_GetLoop(runtime_), async.get(),
                    reinterpret_cast<uv_async_cb>(
                        &PackageRuntime::async_callback)) != 0)
                throw std::runtime_error(
                    "cannot create runtime command queue");
            {
                std::lock_guard lock(mutex_);
                async_ = reinterpret_cast<uv_async_s*>(async.release());
            }

            std::weak_ptr<PackageRuntime> weak = weak_from_this();
            host::register_runtime(
                runtime_,
                host::BindingConfig{
                    package_.manifest.id,
                    package_.version_dir.string(),
                    package_.data_dir.string(),
                    outbound_,
                    [weak](std::function<void(JSContext*)> task) {
                        if (auto owner = weak.lock())
                            (void)owner->post(std::move(task));
                    },
                    request_reload_,
                    endpoint_config_.max_response_body_bytes,
                    endpoint_config_.max_message_bytes,
                });

            const auto entry =
                package_.version_dir / package_.manifest.entry;
            JSValue evaluated =
                TJS_EvalModule(context_, entry.c_str(), true);
            bool evaluation_failed = JS_IsException(evaluated);
            if (!evaluation_failed && JS_IsPromise(evaluated)) {
                JSContext* job_context = nullptr;
                int jobs = 0;
                while (JS_PromiseState(context_, evaluated) ==
                           JS_PROMISE_PENDING &&
                    (jobs = JS_ExecutePendingJob(JS_GetRuntime(context_),
                         &job_context)) > 0) {
                    // Drain initialization microtasks before advertising ready.
                }
                if (jobs < 0) {
                    if (job_context != context_) {
                        JSValue exception = JS_GetException(job_context);
                        JS_Throw(context_, exception);
                    }
                    evaluation_failed = true;
                }
                else if (JS_PromiseState(context_, evaluated) ==
                    JS_PROMISE_REJECTED) {
                    JS_Throw(context_,
                        JS_PromiseResult(context_, evaluated));
                    evaluation_failed = true;
                }
                else if (JS_PromiseState(context_, evaluated) ==
                    JS_PROMISE_PENDING) {
                    JS_ThrowTypeError(context_,
                        "management entry top-level await must settle "
                        "without waiting for I/O");
                    evaluation_failed = true;
                }
            }
            bool stopping = false;
            {
                std::lock_guard lock(mutex_);
                stopping = stopping_;
            }
            if (evaluation_failed) {
                TJS_DumpException(context_);
                fail("management entry failed to evaluate");
            }
            else if (!stopping) {
                state_.store(RuntimeState::running);
            }
            else {
                state_.store(RuntimeState::stopping);
            }
            JS_FreeValue(context_, evaluated);
            ready();

            if (state_.load() == RuntimeState::running)
                (void)TJS_Run(runtime_);
        }
        catch (const std::exception& exception) {
            fail(exception.what());
        }
        catch (...) {
            fail("unexpected package runtime failure");
        }

        // Both notifications are deliberately outside the try block: their
        // implementations are non-throwing, so every owner-thread path has
        // one ready transition, one teardown and one exit transition.
        ready();
        teardown();
        signal_exited();
    }

    void PackageRuntime::teardown() noexcept {
        auto* context = context_;
        auto* runtime = runtime_;
        uv_async_t* async = nullptr;
        {
            std::lock_guard lock(mutex_);
            async = reinterpret_cast<uv_async_t*>(async_);
            async_ = nullptr;
        }

        if (context) {
            try { host::shutdown_runtime(context); }
            catch (...) { fail("package runtime shutdown failed"); }
        }
        if (runtime) {
            try { host::unregister_runtime(runtime); }
            catch (...) { fail("package runtime unregister failed"); }
        }
        if (async &&
            !uv_is_closing(reinterpret_cast<uv_handle_t*>(async))) {
            uv_close(reinterpret_cast<uv_handle_t*>(async),
                [](uv_handle_t* handle) {
                    delete reinterpret_cast<uv_async_t*>(handle);
                });
        }
        fail_queued(CallErrorCode::unavailable, "package runtime stopped");
        finish_graceful_stop();
        if (runtime) TJS_FreeRuntime(runtime);
        runtime_ = nullptr;
        context_ = nullptr;
        if (state_.load() != RuntimeState::failed)
            state_.store(RuntimeState::disabled);
    }

    void PackageRuntime::fail(std::string_view message) noexcept {
        state_.store(RuntimeState::failed);
        try {
            std::lock_guard lock(mutex_);
            if (failure_.empty()) failure_.assign(message);
        }
        catch (...) {
            // State still records the failure if diagnostic allocation fails.
        }
    }

    void PackageRuntime::finish_graceful_stop() noexcept {
        std::vector<StopCompletion> waiters;
        {
            std::lock_guard lock(mutex_);
            if (graceful_stop_finished_) return;
            graceful_stop_finished_ = true;
            waiters.swap(graceful_stop_waiters_);
        }
        for (auto& waiter : waiters) {
            try {
                waiter();
            }
            catch (...) {}
        }
    }

    void PackageRuntime::signal_exited() noexcept {
        std::vector<std::function<void()>> waiters;
        {
            std::lock_guard lock(mutex_);
            if (exited_.exchange(true)) return;
            waiters.swap(exit_waiters_);
        }
        for (auto& waiter : waiters) {
            try { waiter(); }
            catch (...) {}
        }
    }

    void PackageRuntime::drain() {
        std::deque<RuntimeCall> calls;
        std::deque<EndpointCommand> endpoint_commands;
        std::deque<EndpointClose> endpoint_closes;
        std::deque<std::uint64_t> call_cancellations;
        std::deque<std::function<void(JSContext*)>> tasks;
        bool stop = false;
        {
            std::lock_guard lock(mutex_);
            calls.swap(queue_);
            endpoint_commands.swap(endpoint_commands_);
            endpoint_closes.swap(endpoint_closes_);
            call_cancellations.swap(call_cancellations_);
            tasks.swap(tasks_);
            queue_bytes_ = 0;
            endpoint_command_bytes_ = 0;
            stop = stopping_;
        }
        for (auto& task : tasks) {
            try { task(context_); }
            catch (...) {
                fail("runtime completion task failed");
                request_shutdown("runtime completion task failed");
                stop = true;
            }
        }
        if (stop) {
            RuntimeStopGuard stop_runtime(runtime_);
            // Preserve owner-thread ordering for already accepted work. Opens
            // must be registered before their queued close can abort its
            // signal and emit endpoint.close. No new commands are accepted
            // after stopping_ becomes visible.
            for (auto& command : endpoint_commands) {
                try {
                    std::visit(
                        [this](auto&& value) {
                            using Value = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<Value,
                                              OpenEndpoint>) {
                                host::dispatch_endpoint(context_,
                                    std::move(value.request),
                                    std::move(value.sink));
                            }
                            else if constexpr (std::is_same_v<Value,
                                                   EndpointMessage>) {
                                host::dispatch_endpoint_message(context_,
                                    std::move(value));
                            }
                        },
                        std::move(command));
                }
                catch (...) {
                    fail("endpoint command dispatch failed during shutdown");
                    if (auto* open = std::get_if<OpenEndpoint>(&command))
                        emit_endpoint_noexcept(open->sink,
                            stopped_endpoint(open->request.id,
                                "package runtime is stopping"));
                }
            }
            for (auto& close : endpoint_closes) {
                const auto id = close.id;
                try {
                    host::dispatch_endpoint_close(context_, std::move(close));
                }
                catch (...) {
                    fail("endpoint close dispatch failed during shutdown");
                }
                std::lock_guard lock(mutex_);
                pending_endpoint_close_ids_.erase(id);
            }
            for (const auto id : call_cancellations) {
                try { host::dispatch_cancel(context_, id); }
                catch (...) {
                    fail("call cancellation failed during shutdown");
                }
                std::lock_guard lock(mutex_);
                pending_call_cancellation_ids_.erase(id);
            }
            for (auto& call : calls)
                fail_call_noexcept(call, CallErrorCode::unavailable,
                    "package runtime is stopping");
            try { host::shutdown_runtime(context_); }
            catch (...) { fail("package runtime shutdown failed"); }
            /* RuntimeStopGuard makes loop stop mandatory. Keep the command
             * handle allocated until TJS_Run() returns; teardown closes it. */
            return;
        }
        bool dispatch_failed = false;
        for (auto& command : endpoint_commands) {
            try {
                std::visit(
                    [this](auto&& value) {
                        using Value = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<Value, OpenEndpoint>) {
                            const auto now = std::chrono::steady_clock::now();
                            if (value.request.deadline !=
                                    std::chrono::steady_clock::time_point{} &&
                                now >= value.request.deadline) {
                                EndpointResponse response;
                                response.id = value.request.id;
                                response.status = 504;
                                constexpr std::string_view text =
                                    "endpoint request expired in queue";
                                response.body.reserve(text.size());
                                for (const char character : text)
                                    response.body.push_back(
                                        static_cast<std::byte>(character));
                                emit_endpoint_noexcept(value.sink,
                                    std::move(response));
                            }
                            else {
                                host::dispatch_endpoint(context_,
                                    std::move(value.request),
                                    std::move(value.sink));
                            }
                        }
                        else if constexpr (std::is_same_v<Value,
                                               EndpointMessage>) {
                            host::dispatch_endpoint_message(context_,
                                std::move(value));
                        }
                    },
                    std::move(command));
            }
            catch (...) {
                fail("endpoint command dispatch failed");
                dispatch_failed = true;
                if (auto* open = std::get_if<OpenEndpoint>(&command)) {
                    try {
                        emit_endpoint_noexcept(open->sink,
                            stopped_endpoint(open->request.id,
                                "endpoint command dispatch failed"));
                    }
                    catch (...) {}
                }
            }
        }
        // Opens and messages from the same snapshot must run first: otherwise
        // a close racing an open could be observed before the session exists.
        for (auto& close : endpoint_closes) {
            const auto id = close.id;
            try {
                host::dispatch_endpoint_close(context_, std::move(close));
            }
            catch (...) {
                fail("endpoint close dispatch failed");
                dispatch_failed = true;
            }
            std::lock_guard lock(mutex_);
            pending_endpoint_close_ids_.erase(id);
        }
        std::deque<RuntimeCall> lifecycle_calls;
        const auto dispatch_calls = [this, &dispatch_failed](
                                        std::deque<RuntimeCall>& batch) {
            for (auto& call : batch) {
                if (dispatch_failed) {
                    fail_call_noexcept(call, CallErrorCode::unavailable,
                        "package runtime dispatch failed");
                }
                else if (std::chrono::steady_clock::now() >= call.deadline) {
                    fail_call_noexcept(call, CallErrorCode::timeout,
                        "call expired in queue");
                }
                else {
                    try { host::dispatch_call(context_, std::move(call)); }
                    catch (...) {
                        fail("call dispatch failed");
                        dispatch_failed = true;
                        fail_call_noexcept(call, CallErrorCode::unavailable,
                            "package runtime dispatch failed");
                    }
                }
            }
        };
        std::deque<RuntimeCall> ordinary_calls;
        for (auto& call : calls) {
            if (call.id == std::numeric_limits<std::uint64_t>::max())
                lifecycle_calls.push_back(std::move(call));
            else
                ordinary_calls.push_back(std::move(call));
        }
        dispatch_calls(ordinary_calls);
        // Calls from the same snapshot must be registered before cancellation;
        // otherwise a timeout racing an enqueued call could be lost.
        for (const auto id : call_cancellations) {
            // The reserved lifecycle call completes graceful-stop itself. Its
            // cancellation can race only with the forced-stop phase, which
            // makes the whole runtime unavailable immediately afterward.
            if (id == std::numeric_limits<std::uint64_t>::max()) continue;
            try { host::dispatch_cancel(context_, id); }
            catch (...) {
                fail("call cancellation dispatch failed");
                dispatch_failed = true;
            }
            std::lock_guard lock(mutex_);
            pending_call_cancellation_ids_.erase(id);
        }
        // lifecycle.stop is deliberately last. Its synchronous completion may
        // let the control actor finish daemon shutdown, so every cancellation
        // accepted earlier in this batch must already have notified the
        // package's AbortSignal listeners.
        dispatch_calls(lifecycle_calls);
        for (const auto id : call_cancellations) {
            if (id != std::numeric_limits<std::uint64_t>::max()) continue;
            try { host::dispatch_cancel(context_, id); }
            catch (...) {
                fail("lifecycle cancellation dispatch failed");
                dispatch_failed = true;
            }
            std::lock_guard lock(mutex_);
            pending_call_cancellation_ids_.erase(id);
        }
        if (dispatch_failed) {
            RuntimeStopGuard stop_runtime(runtime_);
            request_shutdown("runtime command dispatch failed");
            try { host::shutdown_runtime(context_); }
            catch (...) { fail("package runtime shutdown failed"); }
        }
    }

    void PackageRuntime::fail_queued(CallErrorCode code,
        std::string_view message) noexcept {
        std::deque<RuntimeCall> calls;
        std::deque<EndpointCommand> endpoint_commands;
        std::deque<EndpointClose> endpoint_closes;
        std::deque<std::uint64_t> call_cancellations;
        {
            std::lock_guard lock(mutex_);
            calls.swap(queue_);
            endpoint_commands.swap(endpoint_commands_);
            endpoint_closes.swap(endpoint_closes_);
            call_cancellations.swap(call_cancellations_);
            pending_endpoint_close_ids_.clear();
            pending_call_cancellation_ids_.clear();
            tasks_.clear();
            queue_bytes_ = 0;
            endpoint_command_bytes_ = 0;
        }
        for (auto& call : calls)
            fail_call_noexcept(call, code, message);
        for (auto& command : endpoint_commands) {
            if (auto* open = std::get_if<OpenEndpoint>(&command)) {
                try {
                    emit_endpoint_noexcept(open->sink,
                        stopped_endpoint(open->request.id,
                            std::string(message)));
                }
                catch (...) {
                    emit_endpoint_noexcept(open->sink,
                        stopped_endpoint(open->request.id,
                            "package runtime stopped"));
                }
            }
        }
    }

} // namespace circud
