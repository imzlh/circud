#include <circud/core.hpp>
#include <circud/endpoint_gateway.hpp>
#include <circud/rpc_error.hpp>

#include "json.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <limits>
#include <stdexcept>
#include <thread>

#ifndef CIRCUD_VERSION
#define CIRCUD_VERSION "unknown"
#endif

namespace circud {
    namespace {

        boost::json::value package_json(const StoredPackage& package,
            RuntimeState state,
            std::string_view runtime_failure) {
            auto object = manifest_to_json(package.manifest).as_object();
            object["enabled"] = package.enabled;
            object["runtimeState"] = to_string(state);
            if (!runtime_failure.empty()) object["runtimeError"] = runtime_failure;
            return object;
        }

        std::int64_t epoch_millis(std::chrono::system_clock::time_point value) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                value.time_since_epoch())
                .count();
        }

        boost::json::value snapshot_json(const StatusSnapshot& snapshot) {
            boost::json::object object{
                { "stale", snapshot.stale },
                { "error", snapshot.error },
                { "consecutiveTimeouts", snapshot.consecutive_timeouts },
                { "collectedAtMs", epoch_millis(snapshot.collected_at) },
                { "lastSuccessAtMs", epoch_millis(snapshot.last_success_at) },
            };
            if (snapshot.value) {
                object["value"] = boost::json::object{
                    { "state", snapshot.value->state },
                    { "message", snapshot.value->message },
                };
            }
            else {
                object["value"] = nullptr;
            }
            return object;
        }

        bool valid_status_state(std::string_view state) {
            static constexpr std::array<std::string_view, 6> values{
                "starting", "running", "stopped", "degraded", "failed", "unknown" };
            return std::ranges::find(values, state) != values.end();
        }

        std::string required_string(const boost::json::object& object,
            std::string_view name) {
            const auto* value = object.if_contains(name);
            if (!value || !value->is_string())
                throw std::runtime_error(std::string(name) + " must be a string");
            return std::string(value->as_string());
        }

        std::uint64_t optional_unsigned(const boost::json::object& object,
            std::string_view name, std::uint64_t fallback) {
            const auto* value = object.if_contains(name);
            if (!value) return fallback;
            if (value->is_uint64()) return value->as_uint64();
            if (value->is_int64() && value->as_int64() >= 0)
                return static_cast<std::uint64_t>(value->as_int64());
            throw std::runtime_error(std::string(name) +
                " must be a non-negative integer");
        }

        std::optional<std::string> optional_nonempty_string(
            const boost::json::object& object, std::string_view name) {
            const auto* value = object.if_contains(name);
            if (!value) return std::nullopt;
            if (!value->is_string() || value->as_string().empty())
                throw std::runtime_error(std::string(name) +
                    " must be a non-empty string");
            if (value->as_string().size() > 256)
                throw std::runtime_error(std::string(name) + " is too long");
            return std::string(value->as_string());
        }

        boost::json::value event_json(const ManagementEvent& event) {
            boost::json::object object{
                { "id", event.id },
                { "timestampMs", epoch_millis(event.timestamp) },
                { "level", to_string(event.level) },
                { "category", event.category },
                { "action", event.action },
                { "code", event.code },
                { "message", event.message },
                { "details", event.details },
            };
            object["package"] = event.package_id
                ? boost::json::value(*event.package_id)
                : boost::json::value(nullptr);
            return object;
        }

        void reject_unknown(const boost::json::object& object,
            std::initializer_list<std::string_view> allowed,
            std::string_view scope) {
            for (const auto& item : object) {
                bool known = false;
                for (const auto field : allowed) {
                    if (item.key() == field) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    const auto prefix = scope.empty() ? std::string{}
                        : std::string(scope) + ".";
                    throw std::runtime_error("unknown RPC field: " + prefix +
                        std::string(item.key()));
                }
            }
        }

    } // namespace

    struct Core::PendingCall {
        std::uint64_t id = 0;
        std::string source;
        std::string package_id;
        std::weak_ptr<PackageRuntime> runtime;
        std::function<void(CallResult)> completion;
        std::shared_ptr<boost::asio::steady_timer> timer;
    };

    class Core::RuntimeReaper {
    public:
        struct Retired {
            std::shared_ptr<PackageRuntime> runtime;
            std::function<void()> joined;
            std::string package_id;
            bool armed = false;
        };

        RuntimeReaper() : thread_([this] { run(); }) {}

        ~RuntimeReaper() {
            wait_empty();
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            changed_.notify_one();
            if (thread_.joinable()) thread_.join();
        }

        bool retire(std::shared_ptr<PackageRuntime> runtime,
            std::function<void()> joined = {}) {
            if (!runtime) return false;
            const auto package_id = runtime->package().manifest.id;
            if (!runtime->claim_join()) return false;
            auto* identity = runtime.get();
            {
                std::lock_guard lock(mutex_);
                constexpr std::size_t max_retired_per_package = 1;
                const auto package_count = per_package_.find(package_id);
                // Bound both queued and currently joining generations. A
                // refusal is fail-closed: Core keeps the exact runtime owner,
                // while daemon shutdown still withdraws it and completes its
                // control-side deadline without waiting for this reaper.
                if (retired_.size() + active_ >= kMaxStoredPackages ||
                    (package_count != per_package_.end() &&
                        package_count->second >= max_retired_per_package)) {
                    runtime->release_join_claim();
                    return false;
                }
                try {
                    retired_.emplace(identity, Retired{
                        runtime, std::move(joined), package_id, false });
                    ++per_package_[package_id];
                }
                catch (...) {
                    // Keep the local shared_ptr alive while rolling back the
                    // join claim. Erasing a last owner of a joinable runtime
                    // here would otherwise terminate the process.
                    runtime->release_join_claim();
                    if (const auto found = retired_.find(identity);
                        found != retired_.end())
                        retired_.erase(found);
                    if (const auto count = per_package_.find(package_id);
                        count != per_package_.end() && count->second == 0)
                        per_package_.erase(count);
                    return false;
                }
            }

            bool registered = false;
            try {
                registered = runtime->notify_on_exit([this] {
                    changed_.notify_one();
                });
            }
            catch (...) {
                // Treat observer construction/locking failure exactly like a
                // bounded-admission refusal and roll the whole transfer back.
            }
            {
                std::lock_guard lock(mutex_);
                const auto found = retired_.find(identity);
                if (!registered) {
                    if (found != retired_.end()) retired_.erase(found);
                    const auto count = per_package_.find(package_id);
                    if (count != per_package_.end()) {
                        if (count->second > 1) --count->second;
                        else per_package_.erase(count);
                    }
                    runtime->release_join_claim();
                    if (retired_.empty() && active_ == 0)
                        drained_.notify_all();
                    return false;
                }
                // The entry is invisible to the reaper until exit
                // notification registration has succeeded. Waking after
                // arming also covers a runtime that exited between insertion
                // and notify_on_exit().
                if (found != retired_.end()) found->second.armed = true;
            }
            changed_.notify_one();
            return true;
        }

        bool has_retired(std::string_view package_id) const {
            std::lock_guard lock(mutex_);
            const auto found = per_package_.find(std::string(package_id));
            return found != per_package_.end() && found->second != 0;
        }

        void wait_empty() {
            std::unique_lock lock(mutex_);
            drained_.wait(lock, [this] {
                return retired_.empty() && active_ == 0;
            });
        }

    private:
        void run() noexcept {
            for (;;) {
                Retired retired;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [this] {
                        return stopping_ || std::ranges::any_of(retired_,
                            [](const auto& entry) {
                                return entry.second.armed &&
                                    entry.second.runtime->exited();
                            });
                    });
                    auto found = std::ranges::find_if(retired_,
                        [](const auto& entry) {
                            return entry.second.armed &&
                                entry.second.runtime->exited();
                        });
                    if (found == retired_.end()) {
                        if (stopping_) return;
                        continue;
                    }
                    retired = std::move(found->second);
                    retired_.erase(found);
                    ++active_;
                }
                try {
                    retired.runtime->join();
                }
                catch (...) {
                    // Joining is reclamation, not a daemon-fatal operation.
                }
                {
                    std::lock_guard lock(mutex_);
                    const auto count = per_package_.find(retired.package_id);
                    if (count != per_package_.end()) {
                        if (count->second > 1) --count->second;
                        else per_package_.erase(count);
                    }
                    --active_;
                    if (retired_.empty() && active_ == 0) drained_.notify_all();
                }
                if (retired.joined) {
                    try { retired.joined(); }
                    catch (...) {}
                }
            }
        }

        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::condition_variable drained_;
        std::unordered_map<const PackageRuntime*, Retired> retired_;
        std::unordered_map<std::string, std::size_t> per_package_;
        std::thread thread_;
        std::size_t active_ = 0;
        bool stopping_ = false;
    };

    class Core::BlockingWorker {
    public:
        BlockingWorker() : thread_([this] { run(); }) {}

        ~BlockingWorker() {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            changed_.notify_one();
            if (thread_.joinable()) thread_.join();
        }

        bool post(std::function<void()> task) {
            // Reserve one slot for low-priority durability work. Both queues
            // share a hard total bound; capacities are not additive.
            constexpr std::size_t max_priority_tasks = 63;
            constexpr std::size_t max_queued_tasks = 64;
            std::lock_guard lock(mutex_);
            if (stopping_ ||
                priority_queue_.size() >= max_priority_tasks ||
                priority_queue_.size() + low_priority_queue_.size() >=
                    max_queued_tasks)
                return false;
            priority_queue_.push_back(std::move(task));
            changed_.notify_one();
            return true;
        }

        bool post_low(std::function<void()> task) {
            constexpr std::size_t max_low_priority_tasks = 8;
            constexpr std::size_t max_queued_tasks = 64;
            std::lock_guard lock(mutex_);
            if (stopping_ ||
                low_priority_queue_.size() >= max_low_priority_tasks ||
                priority_queue_.size() + low_priority_queue_.size() >=
                    max_queued_tasks)
                return false;
            low_priority_queue_.push_back(std::move(task));
            changed_.notify_one();
            return true;
        }

    private:
        void run() noexcept {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [this] {
                        return stopping_ || !priority_queue_.empty() ||
                            !low_priority_queue_.empty();
                    });
                    if (stopping_ && priority_queue_.empty() &&
                        low_priority_queue_.empty())
                        return;
                    constexpr std::size_t max_consecutive_priority_tasks = 8;
                    const bool take_low = !low_priority_queue_.empty() &&
                        (priority_queue_.empty() ||
                            consecutive_priority_tasks_ >=
                                max_consecutive_priority_tasks);
                    auto& queue = take_low
                        ? low_priority_queue_ : priority_queue_;
                    task = std::move(queue.front());
                    queue.pop_front();
                    if (take_low) consecutive_priority_tasks_ = 0;
                    else ++consecutive_priority_tasks_;
                }
                try { task(); }
                catch (...) {}
            }
        }

        std::mutex mutex_;
        std::condition_variable changed_;
        std::deque<std::function<void()>> priority_queue_;
        std::deque<std::function<void()>> low_priority_queue_;
        std::thread thread_;
        std::size_t consecutive_priority_tasks_ = 0;
        bool stopping_ = false;
    };

    Core::Core(CoreConfig config)
        : config_(std::move(config)),
        state_lock_(config_.state_dir),
        database_(config_.state_dir / "circud.db"),
        installer_(config_.state_dir),
        events_(config_.events_enabled, config_.event_capacity),
        status_timer_(io_),
        endpoint_gateway_(std::make_unique<EndpointGateway>(*this)),
        reaper_(std::make_unique<RuntimeReaper>()),
        blocking_worker_(std::make_unique<BlockingWorker>()) {
    }

    Core::~Core() {
        stop();
        // Destruction is the conservative boundary: callers must have stopped
        // external producers, and this thread finishes any queued control work
        // before waiting for all reaper-owned joins.
        io_.restart();
        io_.run();
        // Embedders use conservative destruction rather than the daemon's
        // bounded _Exit path. Join the single blocking worker while Core state
        // is still alive, then drain any stale generation completions it
        // posted. This prevents callbacks from outliving the maps they inspect.
        blocking_worker_.reset();
        io_.restart();
        io_.run();
        if (reaper_) reaper_->wait_empty();
    }

    EndpointGateway& Core::endpoint_gateway() noexcept {
        return *endpoint_gateway_;
    }

    const EndpointGateway& Core::endpoint_gateway() const noexcept {
        return *endpoint_gateway_;
    }

    void Core::start() {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (started_) return;
            if (stopping_)
                throw std::runtime_error("circud is still stopping");
            if (!lifecycle_runtimes_.empty())
                throw std::runtime_error(
                    "a previous package runtime generation is still owned");
            if (generation_ != 0 && !io_.stopped())
                throw std::runtime_error(
                    "circud control loop has not finished stopping");
        }
        if (package_operations_.load(std::memory_order_acquire) != 0)
            throw std::runtime_error(
                "a package filesystem operation is still in progress");
        if (database_operations_.load(std::memory_order_acquire) != 0)
            throw std::runtime_error(
                "a package state write is still in progress");
        const auto stored_packages = database_.packages();
        installer_.cleanup_orphans(stored_packages);
        endpoint_gateway_->publish(stored_packages);
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            started_ = true;
            generation = ++generation_;
            io_.restart();
            work_.emplace(boost::asio::make_work_guard(io_));
        }
        // Startup only submits work. A runtime becomes routable after its owner
        // thread reports ready on the control actor.
        restore_packages();
        record_event(EventLevel::info, "daemon", "start",
            "circud started", std::nullopt, "started");
        if (config_.status_polling_enabled)
            boost::asio::post(io_, [this, generation] {
                arm_status_timer(generation);
            });
    }

    void Core::stop() {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        std::uint64_t stop_generation = 0;
        {
            std::lock_guard lock(mutex_);
            if (!started_ || stopping_) return;
            started_ = false;
            stopping_ = true;
            stop_generation = ++generation_;
        }
        boost::asio::post(io_, [this, stop_generation] {
            stop_on_control(stop_generation, [this, stop_generation] {
                if (work_) work_->reset();
                work_.reset();
                {
                    std::lock_guard lock(mutex_);
                    if (generation_ != stop_generation || !stopping_) return;
                    stopping_ = false;
                }
            });
        });
    }

    void Core::restore_packages() {
        for (const auto& package : database_.packages()) {
            if (!package.enabled) continue;
            async_start_package(package.manifest.id);
        }
    }

    void Core::stop_on_control(std::uint64_t stop_generation,
        std::function<void()> completion) {
        status_timer_.cancel();
        std::vector<std::shared_ptr<PackageRuntime>> runtimes;
        std::vector<std::shared_ptr<PendingCall>> pending;
        std::vector<LifecycleCompletion> lifecycle_completions;
        {
            std::lock_guard lock(mutex_);
            runtimes.reserve(runtimes_.size() + lifecycle_runtimes_.size());
            for (auto& [id, runtime] : runtimes_) {
                runtimes.push_back(runtime);
                lifecycle_runtimes_.insert_or_assign(id, runtime);
            }
            for (auto& [id, runtime] : lifecycle_runtimes_) {
                if (std::ranges::none_of(runtimes,
                    [&runtime](const auto& value) {
                        return value.get() == runtime.get();
                    }))
                    runtimes.push_back(runtime);
            }
            runtimes_.clear();
            for (auto& [id, lane] : lifecycle_lanes_) {
                if (lane.timer) lane.timer->cancel();
                if (lane.active && lane.active->completion)
                    lifecycle_completions.push_back(
                        std::move(lane.active->completion));
                for (auto& request : lane.queued)
                    if (request.completion)
                        lifecycle_completions.push_back(
                            std::move(request.completion));
            }
            lifecycle_lanes_.clear();
            for (auto& [id, call] : pending_)
                pending.push_back(std::move(call));
            pending_.clear();
        }
        endpoint_gateway_->reset();
        record_event(EventLevel::info, "daemon", "stop",
            "circud is stopping", std::nullopt, "stopping");
        for (auto& callback : lifecycle_completions) {
            try { callback("circud is stopping"); }
            catch (...) {}
        }
        for (auto& call : pending) {
            call->timer->cancel();
            if (auto runtime = call->runtime.lock()) runtime->cancel(call->id);
            deliver_call(call, CallResult::failure(
                CallErrorCode::unavailable, "circud is stopping"));
        }
        if (runtimes.empty()) {
            completion();
            return;
        }

        auto remaining = std::make_shared<std::size_t>(runtimes.size());
        auto finish_one = [completion = std::move(completion), remaining]() mutable {
            if (--*remaining == 0) completion();
        };
        for (auto& runtime : runtimes) {
            auto timer = std::make_shared<boost::asio::steady_timer>(io_);
            timer->expires_after(runtime->package().manifest.shutdown_timeout);
            auto once = std::make_shared<std::atomic_bool>(false);
            auto finish = [this, runtime, timer, once, finish_one,
                              stop_generation]() mutable {
                if (once->exchange(true)) return;
                timer->cancel();
                endpoint_gateway_->release_runtime(runtime);
                fail_calls_for_runtime(runtime,
                    "package runtime stopped during daemon shutdown");
                const auto package_id = runtime->package().manifest.id;
                (void)retire_runtime(runtime, "shutdown",
                    [this, package_id, stop_generation, runtime] {
                        // stop() is allowed to finish before a package owner
                        // thread has been joined. Only the exact retired owner
                        // may wake the immediately following daemon generation;
                        // a late callback must not cross a later stop/start or
                        // interfere with a replacement runtime.
                        boost::asio::post(io_, [this, package_id,
                            stop_generation, runtime] {
                            bool resume = false;
                            {
                                std::lock_guard lock(mutex_);
                                resume = runtime->exited() && started_ &&
                                    stop_generation !=
                                        std::numeric_limits<std::uint64_t>::max() &&
                                    generation_ == stop_generation + 1 &&
                                    !runtimes_.contains(package_id) &&
                                    !lifecycle_runtimes_.contains(package_id);
                            }
                            if (!resume) return;
                            const auto package = database_.package(package_id);
                            if (package && package->enabled)
                                async_start_package(package_id);
                        });
                    });
                finish_one();
            };
            timer->async_wait([finish](const boost::system::error_code& error) mutable {
                if (!error) finish();
            });
            const bool admitted = runtime->async_graceful_stop(
                "shutdown", [this, finish]() mutable {
                    boost::asio::post(io_, std::move(finish));
                });
            if (!admitted) finish();
        }
    }

    StoredPackage Core::install(const std::filesystem::path& archive) {
        if (io_.get_executor().running_in_this_thread())
            throw std::logic_error(
                "synchronous install cannot run on the control thread");
        std::size_t expected_operations = 0;
        if (!package_operations_.compare_exchange_strong(
                expected_operations, 1, std::memory_order_acq_rel))
            throw std::runtime_error(
                "another package filesystem operation is in progress");
        struct OperationEnd {
            std::atomic<std::size_t>& count;
            ~OperationEnd() { count.store(0, std::memory_order_release); }
        } operation_end{ package_operations_ };
        std::optional<std::string> package_id;
        const auto wait_lifecycle = [this](std::string id,
                                        LifecycleAction action) {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            submit_lifecycle(std::move(id), action,
                [promise](std::optional<std::string> error) mutable {
                    try {
                        if (error) {
                            promise->set_exception(std::make_exception_ptr(
                                std::runtime_error(std::move(*error))));
                        }
                        else {
                            promise->set_value();
                        }
                    }
                    catch (...) {}
                }, true);
            return future;
        };
        try {
            auto package = installer_.install(archive);
            package_id = package.manifest.id;
            auto previous = database_.package(package.manifest.id);
            if (previous) package.enabled = previous->enabled;
            auto candidate_packages = database_.packages();
            candidate_packages.erase(std::remove_if(candidate_packages.begin(),
                candidate_packages.end(), [&](const StoredPackage& candidate) {
                    return candidate.manifest.id == package.manifest.id;
                }), candidate_packages.end());
            candidate_packages.push_back(package);
            try {
                std::vector<PackageManifest> manifests;
                manifests.reserve(candidate_packages.size());
                for (const auto& candidate : candidate_packages)
                    manifests.push_back(candidate.manifest);
                EndpointGateway::validate_scopes(manifests);
            }
            catch (...) {
                installer_.discard_release(package);
                throw;
            }
            bool running = false;
            {
                std::lock_guard lock(mutex_);
                running = started_;
            }
            if (previous && running) {
                try {
                    // Enter the same per-package serial lane as every accepted
                    // lifecycle operation before publishing a new manifest or
                    // deleting an old release. New external lifecycle requests
                    // are fenced by package_operations_.
                    wait_lifecycle(package.manifest.id,
                        LifecycleAction::quiesce).get();
                    previous = database_.package(package.manifest.id);
                    if (!previous)
                        throw std::runtime_error(
                            "package was removed during installation");
                }
                catch (...) {
                    installer_.discard_release(package);
                    throw;
                }
            }
            if (previous && reaper_->has_retired(package.manifest.id)) {
                installer_.discard_release(package);
                throw std::runtime_error(
                    "previous package runtime generation has not exited");
            }
            bool published = false;
            try {
                database_.upsert_package(package);
                published = true;
                if (previous &&
                    previous->version_dir != package.version_dir) {
                    std::lock_guard lock(mutex_);
                    status_snapshots_.erase(package.manifest.id);
                    dirty_status_snapshots_.erase(package.manifest.id);
                }
                if (running && package.enabled) {
                    wait_lifecycle(package.manifest.id,
                        LifecycleAction::start).get();
                }
                endpoint_gateway_->publish(candidate_packages);
            }
            catch (...) {
                bool rolled_back = !published;
                if (published) {
                    try {
                        if (running)
                            wait_lifecycle(package.manifest.id,
                                LifecycleAction::stop).get();
                        if (previous) {
                            database_.upsert_package(*previous);
                            {
                                std::lock_guard lock(mutex_);
                                status_snapshots_.erase(
                                    previous->manifest.id);
                                dirty_status_snapshots_.erase(
                                    previous->manifest.id);
                            }
                            if (running && previous->enabled)
                                wait_lifecycle(previous->manifest.id,
                                    LifecycleAction::start).get();
                        }
                        else {
                            database_.remove_package(package.manifest.id);
                        }
                        rolled_back = true;
                        endpoint_gateway_->publish(database_.packages());
                    }
                    catch (...) {
                        // Preserve both releases if rollback cannot be completed;
                        // the database may still reference the new generation.
                    }
                }
                if (rolled_back && !reaper_->has_retired(package.manifest.id))
                    installer_.discard_release(package);
                throw;
            }
            if (previous && previous->version_dir != package.version_dir)
                installer_.discard_release(*previous);
            try {
                record_event(EventLevel::info, "package",
                    previous ? "upgrade" : "install",
                    previous ? "Package upgraded" : "Package installed",
                    package.manifest.id,
                    previous ? "upgraded" : "installed",
                    boost::json::object{
                        { "version", package.manifest.version } });
            }
            catch (...) {
                // Constructing optional diagnostic details must not turn a
                // committed install into a reported failure.
            }
            return package;
        }
        catch (...) {
            record_event(EventLevel::error, "package", "install",
                "Package installation failed",
                package_id ? std::optional<std::string_view>(*package_id)
                           : std::nullopt,
                "install_failed");
            throw;
        }
    }

    void Core::remove(std::string_view id, bool purge_data) {
        const auto previous = database_.package(id);
        if (!previous) throw std::runtime_error("package not found");
        bool running = false;
        {
            std::lock_guard lock(mutex_);
            running = started_;
        }
        if (running) {
            if (io_.get_executor().running_in_this_thread())
                throw std::logic_error(
                    "synchronous remove cannot run on the control thread");
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            async_remove_package(std::string(id), purge_data,
                [promise](std::optional<std::string> error) mutable {
                    try {
                        if (error) promise->set_exception(
                            std::make_exception_ptr(
                                std::runtime_error(std::move(*error))));
                        else promise->set_value();
                    }
                    catch (...) {}
                });
            future.get();
            return;
        }
        std::size_t expected_operations = 0;
        if (!package_operations_.compare_exchange_strong(
                expected_operations, 1, std::memory_order_acq_rel))
            throw std::runtime_error(
                "another package filesystem operation is in progress");
        struct OperationEnd {
            std::atomic<std::size_t>& count;
            ~OperationEnd() { count.store(0, std::memory_order_release); }
        } operation_end{ package_operations_ };
        try {
            installer_.remove(id, purge_data,
                [this, id] { database_.remove_package(id); });
            endpoint_gateway_->publish(database_.packages());
            record_event(EventLevel::info, "package", "remove",
                purge_data ? "Package and data removed" : "Package removed",
                id, purge_data ? "purged" : "removed");
        }
        catch (...) {
            record_event(EventLevel::error, "package", "remove",
                "Package removal failed", id, "remove_failed");
            database_.upsert_package(*previous);
            endpoint_gateway_->publish(database_.packages());
            if (running && previous->enabled) {
                try {
                    start_package(std::string(id)).get();
                }
                catch (...) {}
            }
            throw;
        }
    }

    void Core::async_start_package(std::string id,
        LifecycleCompletion completion) {
        submit_lifecycle(std::move(id), LifecycleAction::start,
            std::move(completion));
    }

    void Core::async_stop_package(std::string id,
        LifecycleCompletion completion) {
        submit_lifecycle(std::move(id), LifecycleAction::stop,
            std::move(completion));
    }

    void Core::async_reload_package(std::string id,
        LifecycleCompletion completion) {
        submit_lifecycle(std::move(id), LifecycleAction::reload,
            std::move(completion));
    }

    void Core::async_remove_package(std::string id, bool purge_data,
        LifecycleCompletion completion) {
        boost::asio::post(io_, [this, id = std::move(id), purge_data,
            completion = std::move(completion)]() mutable {
            bool started = false;
            {
                std::lock_guard lock(mutex_);
                started = started_;
            }
            if (!started) {
                if (completion) completion("circud is not running");
                return;
            }
            auto previous = database_.package(id);
            if (!previous) {
                if (completion) completion("package not found");
                return;
            }
            if (package_operations_.load(std::memory_order_acquire) != 0) {
                if (completion)
                    completion("another package filesystem operation is in progress");
                return;
            }
            auto& lane = lifecycle_lanes_[id];
            constexpr std::size_t max_queued_lifecycle_commands = 64;
            if (lane.queued.size() >= max_queued_lifecycle_commands) {
                if (completion)
                    completion("too many queued package lifecycle commands");
                return;
            }
            LifecycleRequest request;
            request.action = LifecycleAction::remove;
            request.completion = std::move(completion);
            request.purge_data = purge_data;
            request.previous = std::move(previous);
            lane.queued.push_back(std::move(request));
            drive_lifecycle(id);
        });
    }

    namespace {
        std::future<void> lifecycle_future(
            std::function<void(LifecycleCompletion)> submit) {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            submit([promise](std::optional<std::string> error) mutable {
                try {
                    if (error)
                        promise->set_exception(std::make_exception_ptr(
                            std::runtime_error(std::move(*error))));
                    else
                        promise->set_value();
                }
                catch (...) {}
            });
            return future;
        }
    }

    std::future<void> Core::start_package(std::string id) {
        if (io_.get_executor().running_in_this_thread())
            throw std::logic_error(
                "synchronous lifecycle adapter called on the control thread");
        return lifecycle_future([this, id = std::move(id)](
            LifecycleCompletion completion) mutable {
            async_start_package(std::move(id), std::move(completion));
        });
    }

    std::future<void> Core::stop_package(std::string id) {
        if (io_.get_executor().running_in_this_thread())
            throw std::logic_error(
                "synchronous lifecycle adapter called on the control thread");
        return lifecycle_future([this, id = std::move(id)](
            LifecycleCompletion completion) mutable {
            async_stop_package(std::move(id), std::move(completion));
        });
    }

    std::future<void> Core::reload_package(std::string id) {
        if (io_.get_executor().running_in_this_thread())
            throw std::logic_error(
                "synchronous lifecycle adapter called on the control thread");
        return lifecycle_future([this, id = std::move(id)](
            LifecycleCompletion completion) mutable {
            async_reload_package(std::move(id), std::move(completion));
        });
    }

    void Core::submit_lifecycle(std::string id, LifecycleAction action,
        LifecycleCompletion completion, bool package_operation) {
        boost::asio::post(io_, [this, id = std::move(id), action,
            completion = std::move(completion), package_operation]() mutable {
            bool started = false;
            {
                std::lock_guard lock(mutex_);
                started = started_;
            }
            if (!started) {
                if (completion) completion("circud is not running");
                return;
            }
            if (!package_operation &&
                package_operations_.load(std::memory_order_acquire) != 0) {
                if (completion)
                    completion("a package filesystem operation is in progress");
                return;
            }
            if (!database_.package(id)) {
                if (completion) completion("package not found");
                return;
            }
            auto& lane = lifecycle_lanes_[id];
            constexpr std::size_t max_queued_lifecycle_commands = 64;
            if (lane.queued.size() >= max_queued_lifecycle_commands) {
                if (completion)
                    completion("too many queued package lifecycle commands");
                return;
            }
            LifecycleRequest request;
            request.action = action;
            request.completion = std::move(completion);
            lane.queued.push_back(std::move(request));
            drive_lifecycle(id);
        });
    }

    void Core::drive_lifecycle(const std::string& id) {
        auto found = lifecycle_lanes_.find(id);
        if (found == lifecycle_lanes_.end() || found->second.active ||
            found->second.queued.empty())
            return;
        auto& lane = found->second;
        lane.active.emplace(std::move(lane.queued.front()));
        lane.queued.pop_front();
        if (++next_lifecycle_token_ == 0) ++next_lifecycle_token_;
        lane.token = next_lifecycle_token_;
        try {
            switch (lane.active->action) {
            case LifecycleAction::start:
                persist_lifecycle_enabled(id, true);
                break;
            case LifecycleAction::stop:
                persist_lifecycle_enabled(id, false);
                break;
            case LifecycleAction::reload:
                persist_lifecycle_enabled(id, true);
                break;
            case LifecycleAction::remove:
                begin_package_remove(id);
                break;
            case LifecycleAction::quiesce:
                begin_runtime_retire(id, "upgrade", false);
                break;
            }
        }
        catch (const std::exception& error) {
            finish_lifecycle(id, error.what());
        }
    }

    void Core::persist_lifecycle_enabled(const std::string& id,
        bool enabled) {
        const auto lane = lifecycle_lanes_.find(id);
        if (lane == lifecycle_lanes_.end() || !lane->second.active) return;
        const auto token = lane->second.token;
        std::uint64_t daemon_generation = 0;
        {
            std::lock_guard lock(mutex_);
            daemon_generation = generation_;
        }
        // Restore/start and reload commonly preserve an already enabled
        // package. The authoritative cache read is non-blocking, so avoid a
        // redundant SQLite round trip before starting its owner thread.
        if (const auto package = database_.package(id);
            package && package->enabled == enabled) {
            on_lifecycle_enabled(id, daemon_generation, token, std::nullopt);
            return;
        }
        database_operations_.fetch_add(1, std::memory_order_acq_rel);
        const bool submitted = blocking_worker_->post(
            [this, id, enabled, daemon_generation, token] {
                std::optional<std::string> error;
                try { database_.set_enabled(id, enabled); }
                catch (const std::exception& exception) {
                    error = exception.what();
                }
                try {
                    boost::asio::post(io_, [this, id, daemon_generation,
                        token, error = std::move(error)]() mutable {
                        on_lifecycle_enabled(std::move(id),
                            daemon_generation, token, std::move(error));
                    });
                }
                catch (...) {
                    // Allocation failure while returning to control is fatal
                    // only to this operation; daemon shutdown still remains
                    // independent of the worker.
                }
                database_operations_.fetch_sub(1,
                    std::memory_order_acq_rel);
            });
        if (!submitted) {
            database_operations_.fetch_sub(1, std::memory_order_acq_rel);
            finish_lifecycle(id,
                "database worker queue is full");
        }
    }

    void Core::on_lifecycle_enabled(std::string id,
        std::uint64_t daemon_generation, std::uint64_t token,
        std::optional<std::string> error) {
        LifecycleAction action = LifecycleAction::start;
        {
            std::lock_guard lock(mutex_);
            const auto lane = lifecycle_lanes_.find(id);
            if (!started_ || generation_ != daemon_generation ||
                lane == lifecycle_lanes_.end() || !lane->second.active ||
                lane->second.token != token)
                return;
            action = lane->second.active->action;
        }
        if (error) {
            finish_lifecycle(id, std::move(error));
            return;
        }
        switch (action) {
        case LifecycleAction::start:
            begin_runtime_start(id, false);
            break;
        case LifecycleAction::stop:
            begin_runtime_retire(id, "shutdown", false);
            break;
        case LifecycleAction::reload:
            begin_runtime_retire(id, "reload", true);
            break;
        case LifecycleAction::remove:
        case LifecycleAction::quiesce:
            finish_lifecycle(id,
                "invalid lifecycle persistence phase");
            break;
        }
    }

    void Core::begin_runtime_start(const std::string& id, bool reload) {
        auto package = database_.package(id);
        if (!package) return finish_lifecycle(id, "package not found");
        auto lane = lifecycle_lanes_.find(id);
        if (lane == lifecycle_lanes_.end() || !lane->second.active) return;
        if (reaper_->has_retired(id))
            return finish_lifecycle(id,
                "previous package runtime generation has not exited");
        const auto token = lane->second.token;
        std::uint64_t daemon_generation = 0;
        bool already_running = false;
        {
            std::lock_guard lock(mutex_);
            daemon_generation = generation_;
            if (lifecycle_runtimes_.contains(id))
                return finish_lifecycle(id,
                    "previous package runtime generation is still owned");
            if (auto current = runtimes_.find(id); current != runtimes_.end()) {
                const auto state = current->second->state();
                if (state == RuntimeState::running || state == RuntimeState::stuck)
                    already_running = true;
            }
        }
        if (already_running) return finish_lifecycle(id);

        auto identity = std::make_shared<std::weak_ptr<PackageRuntime>>();
        auto runtime = std::make_shared<PackageRuntime>(
            *package,
            [this, source = id](std::string target_package,
                std::string target, ClonePtr arguments,
                std::chrono::milliseconds timeout,
                std::function<void(CallResult)> completion) mutable {
                route_call(source, std::move(target_package), std::move(target),
                    std::move(arguments), std::nullopt, false, timeout,
                    std::move(completion));
            },
            [this, id, daemon_generation, identity] {
                boost::asio::post(io_, [this, id, daemon_generation, identity] {
                    auto runtime = identity->lock();
                    bool current = false;
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = runtimes_.find(id);
                        current = started_ && generation_ == daemon_generation &&
                            runtime && found != runtimes_.end() &&
                            found->second.get() == runtime.get();
                    }
                    if (current) async_reload_package(id);
                });
            }, config_.endpoints);
        *identity = runtime;
        {
            std::lock_guard lock(mutex_);
            lifecycle_runtimes_[id] = runtime;
        }
        auto start_timer = std::make_shared<boost::asio::steady_timer>(io_);
        start_timer->expires_after(config_.runtime_start_timeout);
        lane->second.timer = start_timer;
        start_timer->async_wait(
            [this, id, daemon_generation, token, runtime, reload](
                const boost::system::error_code& error) mutable {
                if (error) return;
                on_runtime_start_timeout(std::move(id), daemon_generation,
                    token, std::move(runtime), reload);
            });
        try {
            runtime->async_start(
                [this, id, daemon_generation, token, runtime, reload](
                    RuntimeState state) mutable {
                    boost::asio::post(io_, [this, id = std::move(id),
                        daemon_generation, token, runtime = std::move(runtime),
                        reload, state]() mutable {
                        on_runtime_ready(std::move(id), daemon_generation, token,
                            std::move(runtime), reload, state);
                    });
                });
        }
        catch (...) {
            start_timer->cancel();
            std::lock_guard lock(mutex_);
            const auto found = lifecycle_runtimes_.find(id);
            if (found != lifecycle_runtimes_.end() &&
                found->second.get() == runtime.get())
                lifecycle_runtimes_.erase(found);
            throw;
        }
    }

    void Core::begin_runtime_retire(const std::string& id,
        std::string reason, bool then_start) {
        auto lane = lifecycle_lanes_.find(id);
        if (lane == lifecycle_lanes_.end() || !lane->second.active) return;
        const auto token = lane->second.token;
        std::uint64_t daemon_generation = 0;
        std::shared_ptr<PackageRuntime> runtime;
        {
            std::lock_guard lock(mutex_);
            daemon_generation = generation_;
            auto current = runtimes_.find(id);
            if (current != runtimes_.end()) {
                runtime = current->second;
                // Publish the lifecycle owner before withdrawing the running
                // owner. If unordered_map growth throws, the routable table
                // still owns the runtime and the control thread never becomes
                // its accidental blocking destructor.
                lifecycle_runtimes_.insert_or_assign(id, runtime);
                runtimes_.erase(current);
            }
            else if (const auto owner = lifecycle_runtimes_.find(id);
                owner != lifecycle_runtimes_.end()) {
                // A failed or timed-out start remains here if it could not be
                // admitted to the bounded reaper. A later stop/rollback must
                // retire this exact owner; reporting a no-op stop could allow
                // its installed release to be deleted while still in use.
                runtime = owner->second;
            }
        }
        if (!runtime) {
            if (reaper_->has_retired(id)) {
                finish_lifecycle(id,
                    "previous package runtime generation has not exited");
                return;
            }
            if (then_start) begin_runtime_start(id, true);
            else {
                record_event(EventLevel::info, "lifecycle", "stop",
                    "Package stopped", id, "stopped");
                finish_lifecycle(id);
            }
            return;
        }
        endpoint_gateway_->release_runtime(runtime);
        fail_calls_for_runtime(runtime,
            then_start ? "package runtime is reloading"
                       : "package runtime is stopping");
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        timer->expires_after(runtime->package().manifest.shutdown_timeout);
        lane->second.timer = timer;
        lane->second.retirement_started = false;
        timer->async_wait([this, id, daemon_generation, token, runtime,
                              then_start](
                              const boost::system::error_code& error) mutable {
            if (error) return;
            on_runtime_stop_phase(std::move(id), daemon_generation, token,
                std::move(runtime), then_start, true);
        });
        const bool admitted = runtime->async_graceful_stop(std::move(reason),
            [this, id, daemon_generation, token, runtime, then_start]() mutable {
                boost::asio::post(io_, [this, id = std::move(id),
                    daemon_generation, token, runtime = std::move(runtime),
                    then_start]() mutable {
                    on_runtime_stop_phase(std::move(id), daemon_generation,
                        token, std::move(runtime), then_start, false);
                });
            });
        if (!admitted)
            on_runtime_stop_phase(id, daemon_generation, token,
                std::move(runtime), then_start, false);
    }

    void Core::begin_package_remove(const std::string& id) {
        auto lane = lifecycle_lanes_.find(id);
        if (lane == lifecycle_lanes_.end() || !lane->second.active) return;
        const auto token = lane->second.token;
        std::uint64_t daemon_generation = 0;
        std::shared_ptr<PackageRuntime> runtime;
        {
            std::lock_guard lock(mutex_);
            daemon_generation = generation_;
            const auto current = runtimes_.find(id);
            if (current != runtimes_.end()) {
                runtime = current->second;
                // Keep at least one Core owner throughout the transfer. This
                // ordering is important if the lifecycle map must allocate.
                lifecycle_runtimes_.insert_or_assign(id, runtime);
                runtimes_.erase(current);
            }
            else if (const auto owner = lifecycle_runtimes_.find(id);
                owner != lifecycle_runtimes_.end()) {
                // Removal must also account for a failed/timed-out start that
                // could not yet transfer to the reaper. Its release remains
                // live until this exact runtime has been joined.
                runtime = owner->second;
            }
        }
        if (!runtime) {
            if (reaper_->has_retired(id)) {
                finish_lifecycle(id,
                    "previous package runtime generation has not exited");
                return;
            }
            const bool purge_data = lane->second.active->purge_data;
            const auto previous = lane->second.active->previous;
            std::size_t expected_operations = 0;
            if (!package_operations_.compare_exchange_strong(
                    expected_operations, 1, std::memory_order_acq_rel)) {
                finish_lifecycle(id,
                    "another package filesystem operation is in progress");
                return;
            }
            const bool submitted = blocking_worker_->post(
                [this, id, purge_data, previous, daemon_generation, token] {
                    std::optional<std::string> error;
                    try {
                        installer_.remove(id, purge_data,
                            [this, &id] { database_.remove_package(id); });
                    }
                    catch (const std::exception& exception) {
                        error = exception.what();
                        if (previous) {
                            try { database_.upsert_package(*previous); }
                            catch (const std::exception& rollback_error) {
                                error = *error + "; database rollback failed: " +
                                    rollback_error.what();
                            }
                        }
                    }
                    auto completed = [this, id, purge_data, previous,
                        daemon_generation, token,
                        error = std::move(error)]() mutable {
                        bool current = false;
                        {
                            std::lock_guard lock(mutex_);
                            const auto lane = lifecycle_lanes_.find(id);
                            current = started_ &&
                                generation_ == daemon_generation &&
                                lane != lifecycle_lanes_.end() &&
                                lane->second.active &&
                                lane->second.token == token &&
                                lane->second.active->action ==
                                    LifecycleAction::remove;
                        }
                        if (!current) {
                            package_operations_.store(
                                0, std::memory_order_release);
                            return;
                        }
                        on_package_removed(std::move(id), purge_data,
                            std::move(previous), std::move(error));
                    };

                    // A final daemon stop is deliberately not held open by
                    // filesystem work. If it has already revoked this
                    // generation, there may be no control loop left to run a
                    // posted stale completion, so release the restart fence on
                    // this worker. Otherwise enqueue while holding mutex_: a
                    // concurrent stop cannot revoke the generation and queue
                    // its own control task ahead of this completion.
                    bool posted = false;
                    {
                        std::lock_guard lock(mutex_);
                        if (started_ && generation_ == daemon_generation) {
                            try {
                                boost::asio::post(io_, std::move(completed));
                                posted = true;
                            }
                            catch (...) {}
                        }
                    }
                    if (!posted)
                        package_operations_.store(
                            0, std::memory_order_release);
                });
            if (!submitted) {
                package_operations_.store(0, std::memory_order_release);
                finish_lifecycle(id,
                    "package removal worker queue is full");
            }
            return;
        }
        endpoint_gateway_->release_runtime(runtime);
        fail_calls_for_runtime(runtime, "package runtime is being removed");
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        timer->expires_after(runtime->package().manifest.shutdown_timeout);
        lane->second.timer = timer;
        lane->second.retirement_started = false;
        timer->async_wait([this, id, daemon_generation, token, runtime](
                              const boost::system::error_code& error) mutable {
            if (error) return;
            on_runtime_stop_phase(std::move(id), daemon_generation, token,
                std::move(runtime), false, true);
        });
        const bool admitted = runtime->async_graceful_stop("remove",
            [this, id, daemon_generation, token, runtime]() mutable {
                boost::asio::post(io_, [this, id = std::move(id),
                    daemon_generation, token, runtime = std::move(runtime)]() mutable {
                    on_runtime_stop_phase(std::move(id), daemon_generation,
                        token, std::move(runtime), false, false);
                });
        });
        if (!admitted)
            on_runtime_stop_phase(id, daemon_generation, token,
                std::move(runtime), false, false);
    }

    void Core::on_runtime_ready(std::string id,
        std::uint64_t daemon_generation, std::uint64_t token,
        std::shared_ptr<PackageRuntime> runtime, bool reload,
        RuntimeState state) {
        bool current = false;
        std::shared_ptr<boost::asio::steady_timer> timer;
        {
            std::lock_guard lock(mutex_);
            auto lane = lifecycle_lanes_.find(id);
            auto owner = lifecycle_runtimes_.find(id);
            current = started_ && generation_ == daemon_generation &&
                lane != lifecycle_lanes_.end() && lane->second.active &&
                lane->second.token == token &&
                owner != lifecycle_runtimes_.end() &&
                owner->second.get() == runtime.get();
            if (current) timer = lane->second.timer;
            if (current && state == RuntimeState::running) {
                runtimes_[id] = runtime;
                lifecycle_runtimes_.erase(owner);
            }
        }
        if (!current) {
            (void)retire_runtime(runtime, "stale generation");
            return;
        }
        if (timer) timer->cancel();
        if (state != RuntimeState::running) {
            const auto detail = runtime->failure();
            const auto error = detail.empty()
                ? std::string("package runtime failed to start") : detail;
            const auto action = reload ? "reload" : "start";
            const bool retired = retire_runtime(runtime, "failed start",
                [this, id, daemon_generation, token, reload,
                    error]() mutable {
                    boost::asio::post(io_, [this, id, daemon_generation,
                        token, reload, error = std::move(error)]() mutable {
                        bool current = false;
                        {
                            std::lock_guard lock(mutex_);
                            const auto lane = lifecycle_lanes_.find(id);
                            current = started_ &&
                                generation_ == daemon_generation &&
                                lane != lifecycle_lanes_.end() &&
                                lane->second.active &&
                                lane->second.token == token;
                        }
                        if (!current) return;
                        record_event(EventLevel::error, "lifecycle",
                            reload ? "reload" : "start",
                            reload ? "Package runtime failed to reload"
                                   : "Package runtime failed to start",
                            id, reload ? "reload_failed" : "start_failed");
                        finish_lifecycle(id, std::move(error));
                    });
                });
            if (!retired) {
                record_event(EventLevel::error, "lifecycle",
                    action,
                    "Package runtime failed and could not be reclaimed",
                    id, "runtime_reclaim_failed");
                finish_lifecycle(id,
                    error + " and could not be reclaimed");
            }
            return;
        }
        endpoint_gateway_->publish(database_.packages());
        record_event(EventLevel::info, "lifecycle",
            reload ? "reload" : "start",
            reload ? "Package reloaded" : "Package started", id,
            reload ? "reloaded" : "started");
        finish_lifecycle(id);
    }

    void Core::on_runtime_start_timeout(std::string id,
        std::uint64_t daemon_generation, std::uint64_t token,
        std::shared_ptr<PackageRuntime> runtime, bool reload) {
        bool current = false;
        {
            std::lock_guard lock(mutex_);
            const auto lane = lifecycle_lanes_.find(id);
            const auto owner = lifecycle_runtimes_.find(id);
            current = started_ && generation_ == daemon_generation &&
                lane != lifecycle_lanes_.end() && lane->second.active &&
                lane->second.token == token &&
                owner != lifecycle_runtimes_.end() &&
                owner->second.get() == runtime.get();
        }
        if (!current) return;
        endpoint_gateway_->release_runtime(runtime);
        fail_calls_for_runtime(runtime,
            "package runtime start timed out");
        const bool retired = retire_runtime(runtime, "start timed out");
        record_event(EventLevel::error, "lifecycle",
            reload ? "reload" : "start",
            reload ? "Package reload timed out"
                   : "Package start timed out",
            id, reload ? "reload_timeout" : "start_timeout");
        finish_lifecycle(id, retired
            ? "package runtime start timed out"
            : "package runtime start timed out and could not be reclaimed");
    }

    void Core::on_runtime_stop_phase(std::string id,
        std::uint64_t daemon_generation, std::uint64_t token,
        std::shared_ptr<PackageRuntime> runtime, bool then_start,
        bool timed_out) {
        if (!runtime) {
            if (then_start) begin_runtime_start(id, true);
            else if (auto lane = lifecycle_lanes_.find(id);
                lane != lifecycle_lanes_.end() && lane->second.active &&
                lane->second.active->action == LifecycleAction::remove) {
                begin_package_remove(id);
            }
            else finish_lifecycle(id);
            return;
        }
        LifecycleAction action = LifecycleAction::stop;
        bool current = false;
        bool retirement_started = false;
        {
            std::lock_guard lock(mutex_);
            auto lane = lifecycle_lanes_.find(id);
            current = started_ && generation_ == daemon_generation &&
                lane != lifecycle_lanes_.end() && lane->second.active &&
                lane->second.token == token;
            if (current) {
                action = lane->second.active->action;
                retirement_started = lane->second.retirement_started;
                if (!retirement_started)
                    lane->second.retirement_started = true;
            }
        }
        if (!current) {
            (void)retire_runtime(runtime, "stale generation");
            return;
        }

        if (retirement_started) {
            if (!timed_out) return;
            if (action == LifecycleAction::stop) {
                record_event(EventLevel::warning, "lifecycle", "stop",
                    "Package was disabled but its owner thread did not exit",
                    id, "stop_timeout");
                finish_lifecycle(id);
            }
            else {
                record_event(EventLevel::warning, "lifecycle",
                    action == LifecycleAction::remove ? "remove" : "reload",
                    "Package owner thread did not exit before the deadline",
                    id, "runtime_exit_timeout");
                finish_lifecycle(id,
                    "previous package runtime generation did not exit");
            }
            return;
        }

        runtime->cancel(std::numeric_limits<std::uint64_t>::max());
        auto joined = [this, id, daemon_generation, token, then_start] {
            boost::asio::post(io_, [this, id, daemon_generation, token,
                then_start] {
                on_runtime_retired(id, daemon_generation, token, then_start);
            });
        };
        if (!retire_runtime(runtime, then_start ? "reload" : "shutdown",
                std::move(joined))) {
            finish_lifecycle(id,
                "package runtime could not be transferred to the reaper");
            return;
        }
        if (timed_out) {
            if (action == LifecycleAction::stop) {
                record_event(EventLevel::warning, "lifecycle", "stop",
                    "Package was disabled but its owner thread did not exit",
                    id, "stop_timeout");
                finish_lifecycle(id);
            }
            else {
                finish_lifecycle(id,
                    "previous package runtime generation did not exit");
            }
        }
    }

    void Core::on_runtime_retired(std::string id,
        std::uint64_t daemon_generation, std::uint64_t token,
        bool then_start) {
        bool current = false;
        LifecycleAction action = LifecycleAction::stop;
        {
            std::lock_guard lock(mutex_);
            const auto lane = lifecycle_lanes_.find(id);
            current = started_ && generation_ == daemon_generation &&
                lane != lifecycle_lanes_.end() && lane->second.active &&
                lane->second.token == token;
            if (current) action = lane->second.active->action;
        }
        if (!current) return;
        if (const auto lane = lifecycle_lanes_.find(id);
            lane != lifecycle_lanes_.end() && lane->second.timer)
            lane->second.timer->cancel();
        if (action == LifecycleAction::remove) begin_package_remove(id);
        else if (then_start) begin_runtime_start(id, true);
        else {
            record_event(EventLevel::info, "lifecycle", "stop",
                "Package stopped", id, "stopped");
            finish_lifecycle(id);
        }
    }

    void Core::on_package_removed(std::string id, bool purge_data,
        std::optional<StoredPackage> previous,
        std::optional<std::string> error) {
        if (error) {
            (void)previous;
            try { endpoint_gateway_->publish(database_.packages()); }
            catch (...) {}
            record_event(EventLevel::error, "package", "remove",
                "Package removal failed", id, "remove_failed");
            // Release the filesystem fence only after rollback and route
            // publication are complete, but before invoking user completion.
            package_operations_.store(0, std::memory_order_release);
            finish_lifecycle(id, std::move(error));
            return;
        }
        try {
            endpoint_gateway_->publish(database_.packages());
            {
                std::lock_guard lock(mutex_);
                status_snapshots_.erase(id);
                dirty_status_snapshots_.erase(id);
            }
            record_event(EventLevel::info, "package", "remove",
                purge_data ? "Package and data removed" : "Package removed",
                id, purge_data ? "purged" : "removed");
        }
        catch (const std::exception& exception) {
            package_operations_.store(0, std::memory_order_release);
            finish_lifecycle(id, exception.what());
            return;
        }
        catch (...) {
            package_operations_.store(0, std::memory_order_release);
            finish_lifecycle(id,
                "package removal completed but route publication failed");
            return;
        }
        package_operations_.store(0, std::memory_order_release);
        finish_lifecycle(id);
    }

    bool Core::retire_runtime(
        const std::shared_ptr<PackageRuntime>& runtime, std::string reason,
        std::function<void()> joined) {
        if (!runtime) return true;
        runtime->request_shutdown(std::move(reason));
        if (!reaper_->retire(runtime, std::move(joined))) return false;
        {
            std::lock_guard lock(mutex_);
            const auto found = lifecycle_runtimes_.find(
                runtime->package().manifest.id);
            if (found != lifecycle_runtimes_.end() &&
                found->second.get() == runtime.get())
                lifecycle_runtimes_.erase(found);
        }
        return true;
    }

    void Core::finish_lifecycle(const std::string& id,
        std::optional<std::string> error) {
        auto found = lifecycle_lanes_.find(id);
        if (found == lifecycle_lanes_.end() || !found->second.active) return;
        if (found->second.timer) found->second.timer->cancel();
        auto completion = std::move(found->second.active->completion);
        found->second.active.reset();
        found->second.timer.reset();
        found->second.retirement_started = false;
        const bool empty = found->second.queued.empty();
        if (empty) lifecycle_lanes_.erase(found);
        if (completion) {
            try { completion(std::move(error)); }
            catch (...) {}
        }
        if (!empty) drive_lifecycle(id);
    }

    std::vector<StoredPackage> Core::packages() const {
        return database_.packages();
    }

    RuntimeState Core::runtime_state(std::string_view id) const {
        std::lock_guard lock(mutex_);
        auto found = runtimes_.find(std::string(id));
        if (found != runtimes_.end()) return found->second->state();
        auto lifecycle = lifecycle_runtimes_.find(std::string(id));
        return lifecycle == lifecycle_runtimes_.end()
            ? RuntimeState::disabled : lifecycle->second->state();
    }

    std::string Core::runtime_failure(std::string_view id) const {
        std::lock_guard lock(mutex_);
        auto found = runtimes_.find(std::string(id));
        if (found != runtimes_.end()) return found->second->failure();
        auto lifecycle = lifecycle_runtimes_.find(std::string(id));
        return lifecycle == lifecycle_runtimes_.end()
            ? std::string{} : lifecycle->second->failure();
    }

    std::optional<StatusSnapshot> Core::status(std::string_view id) const {
        {
            std::lock_guard lock(mutex_);
            const auto found = status_snapshots_.find(std::string(id));
            if (found != status_snapshots_.end()) return found->second;
        }
        return database_.snapshot(id);
    }

    EventPage Core::management_events(const EventQuery& query) const {
        return events_.list(query);
    }

    std::future<CallResult> Core::call(
        std::string source, std::string package_id, std::string target,
        std::string json_arguments, std::chrono::milliseconds timeout) {
        auto promise = std::make_shared<std::promise<CallResult>>();
        auto future = promise->get_future();
        route_call(std::move(source), std::move(package_id), std::move(target), {},
            std::move(json_arguments), true, timeout,
            [promise](CallResult result) mutable {
                promise->set_value(std::move(result));
            });
        return future;
    }

    void Core::route_call(std::string source, std::string package_id,
        std::string target, ClonePtr arguments,
        std::optional<std::string> json_arguments, bool wants_json,
        std::chrono::milliseconds timeout,
        std::function<void(CallResult)> completion) {
        struct Request {
            std::string source;
            std::string package_id;
            std::string target;
            ClonePtr arguments;
            std::optional<std::string> json_arguments;
            bool wants_json = false;
            std::chrono::milliseconds timeout;
        };
        auto request = std::make_shared<Request>(Request{
            std::move(source), std::move(package_id), std::move(target),
            std::move(arguments), std::move(json_arguments), wants_json,
            std::clamp(timeout, std::chrono::milliseconds(1),
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           kMaxCallTimeout)) });

        const auto id = next_call_id_.fetch_add(1);
        auto pending = std::make_shared<PendingCall>();
        pending->id = id;
        pending->source = request->source;
        pending->package_id = request->package_id;
        pending->completion = std::move(completion);
        pending->timer = std::make_shared<boost::asio::steady_timer>(io_);
        pending->timer->expires_after(request->timeout);
        std::optional<CallResult> immediate_failure;
        {
            std::lock_guard lock(mutex_);
            if (!started_) {
                immediate_failure = CallResult::failure(
                    CallErrorCode::unavailable, "circud is not running");
            }
            else if (pending_.size() >= kMaxPendingCalls) {
                immediate_failure = CallResult::failure(
                    CallErrorCode::overloaded, "too many pending calls");
            }
            else {
                /* Install the wait before publishing the PendingCall. stop() can
                 * take it immediately after this lock is released and cancel the
                 * timer without racing timer setup. Asio never invokes the
                 * completion inline, and the handler also takes mutex_. */
                pending->timer->async_wait(
                    [this, id](const boost::system::error_code& error) {
                        if (error) return;
                        std::shared_ptr<PendingCall> timed_out;
                        {
                            std::lock_guard lock(mutex_);
                            auto found = pending_.find(id);
                            if (found == pending_.end()) return;
                            timed_out = found->second;
                            pending_.erase(found);
                        }
                        if (auto runtime = timed_out->runtime.lock())
                            runtime->cancel(id);
                        deliver_call(timed_out, CallResult::failure(
                            CallErrorCode::timeout, "call timed out"));
                    });
                pending_[id] = pending;
            }
        }
        if (immediate_failure) {
            deliver_call(pending, std::move(*immediate_failure));
            return;
        }

        boost::asio::post(io_, [this, id, pending, request]() mutable {
            std::shared_ptr<PackageRuntime> runtime;
            std::optional<CallResult> failure;
            {
                std::lock_guard lock(mutex_);
                if (!started_ || !pending_.contains(id)) return;
                auto found = runtimes_.find(request->package_id);
                if (found == runtimes_.end()) {
                    failure = CallResult::failure(
                        CallErrorCode::not_found, "target package is not running");
                }
                else {
                    runtime = found->second;
                    const auto runtime_state = runtime->state();
                    if (runtime_state != RuntimeState::running &&
                        runtime_state != RuntimeState::stuck) {
                        failure = CallResult::failure(
                            CallErrorCode::unavailable,
                            "target package runtime is " +
                            std::string(to_string(runtime_state)),
                            std::string(to_string(runtime_state)));
                    }
                    else if (request->source != "core" &&
                        request->source != "web" &&
                        !runtime->package().manifest.accept_package_calls) {
                        failure = CallResult::failure(
                            CallErrorCode::package_calls_disabled,
                            "target package does not accept external package calls");
                    }
                    else {
                        pending->runtime = runtime;
                    }
                }
            }
            if (failure) {
                finish_call(id, std::move(*failure));
                return;
            }

            RuntimeCall call;
            call.id = id;
            call.source = std::move(request->source);
            call.target = std::move(request->target);
            call.arguments = std::move(request->arguments);
            call.json_arguments = std::move(request->json_arguments);
            call.wants_json = request->wants_json;
            call.deadline = std::chrono::steady_clock::now() + request->timeout;
            call.complete = [this, id](CallResult result) mutable {
                finish_call(id, std::move(result));
                };
            if (!runtime->enqueue(std::move(call))) {
                finish_call(id, CallResult::failure(
                    CallErrorCode::overloaded,
                    "package runtime queue is unavailable"));
            }
            });
    }

    void Core::finish_call(std::uint64_t id, CallResult result) {
        auto shared = std::make_shared<CallResult>(std::move(result));
        boost::asio::post(io_, [this, id, shared]() mutable {
            std::shared_ptr<PendingCall> pending;
            {
                std::lock_guard lock(mutex_);
                auto found = pending_.find(id);
                if (found == pending_.end()) return;
                pending = found->second;
                pending_.erase(found);
            }
            pending->timer->cancel();
            deliver_call(pending, std::move(*shared));
            });
    }

    void Core::fail_calls_for_runtime(
        const std::shared_ptr<PackageRuntime>& runtime,
        std::string message) {
        if (!runtime) return;
        std::vector<std::shared_ptr<PendingCall>> failed;
        {
            std::lock_guard lock(mutex_);
            for (auto found = pending_.begin(); found != pending_.end();) {
                const auto owner = found->second->runtime.lock();
                if (!owner || owner.get() != runtime.get()) {
                    ++found;
                    continue;
                }
                failed.push_back(std::move(found->second));
                found = pending_.erase(found);
            }
        }
        for (auto& call : failed) {
            call->timer->cancel();
            runtime->cancel(call->id);
            deliver_call(call, CallResult::failure(
                CallErrorCode::unavailable, message));
        }
    }

    void Core::deliver_call(const std::shared_ptr<PendingCall>& call,
        CallResult result) noexcept {
        try {
            record_call_failure(*call, result);
            call->completion(std::move(result));
        }
        catch (...) {
            // A consumer callback must not terminate the Asio thread or abort
            // delivery to the remaining calls during shutdown.
        }
    }

    void Core::record_event(EventLevel level, std::string_view category,
        std::string_view action, std::string_view message,
        std::optional<std::string_view> package_id, std::string_view code,
        boost::json::object details) {
        try {
            ManagementEventInput event;
            event.level = level;
            event.category = category;
            if (package_id) event.package_id = *package_id;
            event.action = action;
            event.code = code;
            event.message = message;
            event.details = std::move(details);
            events_.append(std::move(event));
        }
        catch (...) {
            // Diagnostics must never change the outcome of lifecycle or call
            // operations, including under memory pressure.
        }
    }

    void Core::record_call_failure(const PendingCall& call,
        const CallResult& result) {
        try {
            if (!result.error || call.source == "core") return;
            const auto level = result.error->code == CallErrorCode::remote_error
                ? EventLevel::error
                : EventLevel::warning;
            const auto origin = call.source == "web" ? "web" : "package";
            record_event(level, "rpc", "call", "Package call failed",
                call.package_id, to_string(result.error->code),
                boost::json::object{ { "origin", origin } });
        }
        catch (...) {
            // A diagnostic allocation failure must not strand a call future.
        }
    }

    void Core::arm_status_timer(std::uint64_t generation) {
        {
            std::lock_guard lock(mutex_);
            if (!started_ || generation_ != generation) return;
        }
        status_timer_.expires_after(config_.status_interval);
        status_timer_.async_wait([this, generation](
            const boost::system::error_code& error) {
            if (error) return;
            {
                std::lock_guard lock(mutex_);
                if (!started_ || generation_ != generation) return;
            }
            poll_statuses();
            arm_status_timer(generation);
            });
    }

    void Core::poll_statuses() {
        std::vector<std::string> ids;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, runtime] : runtimes_) ids.push_back(id);
        }
        for (auto& id : ids) collect_status(std::move(id), [](StatusSnapshot) {});
    }

    void Core::collect_status(std::string package_id,
        std::function<void(StatusSnapshot)> completion) {
            constexpr std::size_t max_status_waiters_per_package = 64;
            constexpr std::size_t max_status_waiters = 256;
            bool overloaded = false;
            {
                std::lock_guard lock(mutex_);
                auto found = status_waiters_.find(package_id);
                const auto package_waiters = found == status_waiters_.end()
                    ? 0 : found->second.size();
                if (package_waiters >= max_status_waiters_per_package ||
                    status_waiter_count_ >= max_status_waiters) {
                    overloaded = true;
                }
                else {
                    const bool inserted = found == status_waiters_.end();
                    if (inserted) {
                        found = status_waiters_.try_emplace(package_id).first;
                    }
                    found->second.push_back(std::move(completion));
                    ++status_waiter_count_;
                    if (!inserted) return;
                }
            }
            if (overloaded) {
                StatusSnapshot snapshot;
                try {
                    snapshot = status(package_id)
                        .value_or(StatusSnapshot{});
                }
                catch (...) {}
                snapshot.stale = true;
                snapshot.error = "too many pending status refreshes";
                snapshot.collected_at = std::chrono::system_clock::now();
                try { completion(std::move(snapshot)); }
                catch (...) {}
                return;
            }
            const auto routed_package = package_id;
            std::shared_ptr<PackageRuntime> status_runtime;
            {
                std::lock_guard lock(mutex_);
                const auto found = runtimes_.find(package_id);
                if (found != runtimes_.end()) status_runtime = found->second;
            }
            const auto version_dir = status_runtime
                ? status_runtime->package().version_dir
                : std::filesystem::path{};
            route_call("core", routed_package, "status.get", {}, std::string("null"), true,
                config_.status_timeout,
                [this, package_id = std::move(package_id), version_dir,
                    status_runtime = std::move(status_runtime)](
                        CallResult result) mutable {
                    StatusSnapshot snapshot;
                    bool current_generation = false;
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = runtimes_.find(package_id);
                        current_generation = status_runtime &&
                            found != runtimes_.end() &&
                            found->second.get() == status_runtime.get();
                    }
                    const auto current_package = database_.package(package_id);
                    current_generation = current_generation &&
                        current_package &&
                        current_package->version_dir == version_dir;
                    if (!current_generation) {
                        snapshot = status(package_id)
                            .value_or(StatusSnapshot{});
                        snapshot.stale = true;
                        snapshot.error =
                            "package runtime generation changed during status collection";
                        snapshot.collected_at =
                            std::chrono::system_clock::now();
                        deliver_status(std::move(package_id),
                            std::move(snapshot));
                        return;
                    }
                    try {
                        const auto previous = status(package_id);
                        snapshot = previous.value_or(StatusSnapshot{});
                        snapshot.collected_at = std::chrono::system_clock::now();
                        if (result.error) {
                            snapshot.stale = true;
                            snapshot.error = result.error->message;
                            if (result.error->code == CallErrorCode::timeout)
                                ++snapshot.consecutive_timeouts;
                            else
                                snapshot.consecutive_timeouts = 0;
                        }
                        else {
                            try {
                                const auto value =
                                    parse_json(result.json.value_or("null"));
                                if (!value.is_object())
                                    throw std::runtime_error(
                                        "status response must be an object");
                                const auto& object = value.as_object();
                                StatusValue status;
                                status.state = required_string(object, "state");
                                if (!valid_status_state(status.state))
                                    throw std::runtime_error(
                                        "invalid status state");
                                if (const auto* message =
                                    object.if_contains("message")) {
                                    if (!message->is_string())
                                        throw std::runtime_error(
                                            "status message must be a string");
                                    status.message =
                                        std::string(message->as_string());
                                }
                                snapshot.value = std::move(status);
                                snapshot.stale = false;
                                snapshot.error.clear();
                                snapshot.last_success_at = snapshot.collected_at;
                                snapshot.consecutive_timeouts = 0;
                            }
                            catch (const std::exception& error) {
                                snapshot.stale = true;
                                snapshot.error = error.what();
                                snapshot.consecutive_timeouts = 0;
                            }
                        }
                        if (status_runtime) {
                            if (snapshot.consecutive_timeouts >= 3)
                                status_runtime->mark_stuck();
                            else if (!snapshot.stale)
                                status_runtime->clear_stuck();
                        }
                        if (snapshot.stale &&
                            (!previous || !previous->stale)) {
                            record_event(EventLevel::warning, "status", "stale",
                                "Package status became stale", package_id,
                                "status_stale");
                        }
                        if (previous && previous->stale && !snapshot.stale) {
                            record_event(EventLevel::info, "status", "recover",
                                "Package status recovered", package_id,
                                "status_recovered");
                        }
                        if (snapshot.consecutive_timeouts >= 3 &&
                            (!previous || previous->consecutive_timeouts < 3)) {
                            record_event(EventLevel::warning, "status", "stuck",
                                "Package stopped responding to status checks",
                                package_id, "status_stuck");
                        }
                    }
                    catch (...) {
                        snapshot.collected_at = std::chrono::system_clock::now();
                        snapshot.stale = true;
                        snapshot.error = "status collection failed";
                        record_event(EventLevel::warning, "status", "collect",
                            "Package status collection failed", package_id,
                            "status_collection_failed");
                    }
                    try {
                        const auto current = database_.package(package_id);
                        if (current && current->version_dir == version_dir) {
                            {
                                std::lock_guard lock(mutex_);
                                status_snapshots_.insert_or_assign(
                                    package_id, snapshot);
                            }
                            if (!queue_status_persistence(package_id,
                                    version_dir, snapshot)) {
                                record_event(EventLevel::warning, "status",
                                    "persist",
                                    "Status persistence queue is unavailable",
                                    package_id,
                                    "status_persistence_overloaded");
                            }
                        }
                    }
                    catch (...) {
                        // A fresh status result must still reach all coalesced
                        // waiters if caching/persistence allocation fails.
                    }
                    deliver_status(std::move(package_id), std::move(snapshot));
                });
    }

    bool Core::queue_status_persistence(std::string package_id,
        std::filesystem::path version_dir, StatusSnapshot snapshot) {
        bool schedule = false;
        {
            std::lock_guard lock(mutex_);
            if (!dirty_status_snapshots_.contains(package_id) &&
                dirty_status_snapshots_.size() >= kMaxStoredPackages)
                return false;
            dirty_status_snapshots_.insert_or_assign(std::move(package_id),
                PendingStatusPersistence{
                    std::move(version_dir), std::move(snapshot) });
            if (!status_persistence_scheduled_) {
                status_persistence_scheduled_ = true;
                schedule = true;
            }
        }
        if (!schedule) return true;
        if (blocking_worker_->post_low(
                [this] { drain_status_persistence(); }))
            return true;
        std::lock_guard lock(mutex_);
        status_persistence_scheduled_ = false;
        return false;
    }

    void Core::drain_status_persistence() noexcept {
        std::string package_id;
        PendingStatusPersistence pending;
        {
            std::lock_guard lock(mutex_);
            if (dirty_status_snapshots_.empty()) {
                status_persistence_scheduled_ = false;
                return;
            }
            auto node = dirty_status_snapshots_.extract(
                dirty_status_snapshots_.begin());
            package_id = std::move(node.key());
            pending = std::move(node.mapped());
        }
        try {
            (void)database_.save_snapshot(package_id,
                pending.version_dir, pending.snapshot);
        }
        catch (...) {
            // Runtime status remains available in memory. Persistence is
            // best-effort and must never block control or daemon exit.
        }

        bool reschedule = false;
        {
            std::lock_guard lock(mutex_);
            if (dirty_status_snapshots_.empty())
                status_persistence_scheduled_ = false;
            else
                reschedule = true;
        }
        if (reschedule && !blocking_worker_->post_low(
                [this] { drain_status_persistence(); })) {
            std::lock_guard lock(mutex_);
            status_persistence_scheduled_ = false;
        }
    }

    void Core::deliver_status(std::string package_id,
        StatusSnapshot snapshot) noexcept {
        std::vector<std::function<void(StatusSnapshot)>> waiters;
        {
            std::lock_guard lock(mutex_);
            auto found = status_waiters_.find(package_id);
            if (found == status_waiters_.end()) return;
            status_waiter_count_ -= found->second.size();
            waiters = std::move(found->second);
            status_waiters_.erase(found);
        }
        for (auto& waiter : waiters) {
            try {
                waiter(snapshot);
            }
            catch (...) {}
        }
    }

    std::future<StatusSnapshot> Core::refresh_status(std::string package_id) {
        auto promise = std::make_shared<std::promise<StatusSnapshot>>();
        auto future = promise->get_future();
        collect_status(std::move(package_id), [promise](StatusSnapshot snapshot) mutable {
            promise->set_value(std::move(snapshot));
            });
        return future;
    }

    void Core::async_handle_rpc(boost::json::value request,
        std::function<void(RpcOutcome)> completion) {
        if (!completion)
            throw std::invalid_argument("RPC completion must not be empty");

        struct CompletionState {
            std::atomic_bool completed{ false };
            std::function<void(RpcOutcome)> completion;

            explicit CompletionState(
                std::function<void(RpcOutcome)> callback)
                : completion(std::move(callback)) {}
        };
        auto completion_state = std::make_shared<CompletionState>(
            std::move(completion));
        auto complete = [completion_state](RpcOutcome outcome) noexcept {
            if (completion_state->completed.exchange(true)) return;
            try {
                completion_state->completion(std::move(outcome));
            }
            catch (...) {
                // A consumer callback must not escape into the runtime or the
                // main I/O loop. The exchange above still guarantees one shot.
            }
        };

        try {
            if (!request.is_object())
                throw std::runtime_error("request must be an object");
            const auto& object = request.as_object();
            reject_unknown(object, { "method", "params" }, "");
            const auto method = required_string(object, "method");
            const boost::json::object empty;
            const auto* params_value = object.if_contains("params");
            const auto& params = params_value
                ? (params_value->is_object()
                    ? params_value->as_object()
                    : throw std::runtime_error("params must be an object"))
                : empty;

            if (method == "system.info") {
                reject_unknown(params, {}, "params");
                complete(boost::json::object{
                    { "version", CIRCUD_VERSION },
                    { "apiVersion", 1 },
                    { "limits", boost::json::object{
                        { "rpcBodyBytes", config_.http_max_body_bytes },
                        { "uploadBytes", config_.http_max_upload_bytes },
                        { "staticFileBytes", config_.http_max_static_file_bytes },
                        { "defaultCallTimeoutMs",
                          config_.default_call_timeout.count() },
                        { "maxCallTimeoutMs",
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              kMaxCallTimeout).count() } } },
                    { "statusPolling", boost::json::object{
                        { "enabled", config_.status_polling_enabled },
                        { "intervalMs", config_.status_interval.count() },
                        { "timeoutMs", config_.status_timeout.count() } } },
                    { "events", boost::json::object{
                        { "enabled", config_.events_enabled },
                        { "capacity", config_.event_capacity },
                        { "maxQueryLimit", kMaxEventQueryLimit },
                        { "persistent", false } } },
                });
                return;
            }

            if (method == "events.list") {
                reject_unknown(params,
                    { "afterId", "limit", "level", "category", "package" },
                    "params");
                EventQuery query;
                query.after_id = optional_unsigned(params, "afterId", 0);
                const auto limit = optional_unsigned(
                    params, "limit", kDefaultEventQueryLimit);
                if (limit == 0 || limit > kMaxEventQueryLimit)
                    throw std::runtime_error("limit must be 1.." +
                        std::to_string(kMaxEventQueryLimit));
                query.limit = static_cast<std::size_t>(limit);
                if (const auto level = optional_nonempty_string(params, "level")) {
                    query.level = event_level_from_string(*level);
                    if (!query.level)
                        throw std::runtime_error(
                            "level must be info, warning, or error");
                }
                query.category = optional_nonempty_string(params, "category");
                query.package_id = optional_nonempty_string(params, "package");
                const auto page = management_events(query);
                boost::json::array event_values;
                event_values.reserve(page.events.size());
                for (const auto& event : page.events)
                    event_values.push_back(event_json(event));
                complete(boost::json::object{
                    { "events", std::move(event_values) },
                    { "journalId", page.journal_id },
                    { "oldestId", page.oldest_id },
                    { "latestId", page.latest_id },
                    { "nextAfterId", page.next_after_id },
                    { "droppedBeforeId", page.dropped_before_id },
                    { "gap", page.gap },
                    { "reset", page.reset },
                    { "hasMore", page.has_more },
                });
                return;
            }

            if (method == "packages.list") {
                reject_unknown(params, {}, "params");
                boost::json::array result;
                for (const auto& package : packages()) {
                    const auto state = runtime_state(package.manifest.id);
                    auto value = package_json(
                        package, state, runtime_failure(package.manifest.id));
                    auto& package_object = value.as_object();
                    if (const auto snapshot = status(package.manifest.id))
                        package_object["status"] = snapshot_json(*snapshot);
                    else
                        package_object["status"] = nullptr;
                    result.push_back(std::move(value));
                }
                complete(std::move(result));
                return;
            }
            if (method == "package.remove") {
                reject_unknown(params, { "id", "purgeData" }, "params");
                const auto id = required_string(params, "id");
                if (!database_.package(id))
                    throw RpcError("not_found", "package not found");
                bool purge = false;
                if (const auto* value = params.if_contains("purgeData")) {
                    if (!value->is_bool())
                        throw std::runtime_error("purgeData must be boolean");
                    purge = value->as_bool();
                }
                async_remove_package(id, purge,
                    [complete](std::optional<std::string> error) mutable {
                        if (error) {
                            complete(RpcFailure{
                                "conflict", std::move(*error) });
                            return;
                        }
                        complete(boost::json::object{ { "ok", true } });
                    });
                return;
            }
            if (method == "package.start" || method == "package.stop" ||
                method == "package.reload") {
                reject_unknown(params, { "id" }, "params");
                const auto id = required_string(params, "id");
                if (!database_.package(id))
                    throw RpcError("not_found", "package not found");
                auto finish = [this, id, complete](
                    std::optional<std::string> error) mutable {
                    if (error) {
                        complete(RpcFailure{ "conflict", std::move(*error) });
                        return;
                    }
                    complete(boost::json::object{
                        { "ok", true },
                        { "runtimeState", to_string(runtime_state(id)) },
                    });
                };
                if (method == "package.start")
                    async_start_package(id, std::move(finish));
                else if (method == "package.stop")
                    async_stop_package(id, std::move(finish));
                else
                    async_reload_package(id, std::move(finish));
                return;
            }
            if (method == "status.get") {
                reject_unknown(params, { "id", "refresh" }, "params");
                const auto id = required_string(params, "id");
                bool refresh = false;
                if (const auto* value = params.if_contains("refresh")) {
                    if (!value->is_bool())
                        throw std::runtime_error("refresh must be boolean");
                    refresh = value->as_bool();
                }
                if (!database_.package(id))
                    throw RpcError("not_found", "package not found");
                if (refresh) {
                    collect_status(id,
                        [complete](StatusSnapshot snapshot) mutable {
                            try {
                                complete(snapshot_json(snapshot));
                            }
                            catch (const std::exception& error) {
                                complete(RpcFailure{
                                    "bad_request", error.what() });
                            }
                        });
                    return;
                }
                const auto snapshot = status(id);
                complete(snapshot ? snapshot_json(*snapshot)
                                  : boost::json::value(nullptr));
                return;
            }
            if (method == "call") {
                reject_unknown(params,
                    { "package", "target", "arguments", "timeoutMs" },
                    "params");
                auto package = required_string(params, "package");
                auto target = required_string(params, "target");
                std::string arguments = "null";
                if (const auto* value = params.if_contains("arguments"))
                    arguments = serialize_json(*value);
                std::chrono::milliseconds timeout = config_.default_call_timeout;
                if (const auto* value = params.if_contains("timeoutMs")) {
                    if (!value->is_int64() || value->as_int64() <= 0)
                        throw std::runtime_error("timeoutMs must be positive");
                    if (value->as_int64() >
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxCallTimeout).count())
                        throw std::runtime_error(
                            "timeoutMs exceeds five-minute limit");
                    timeout = std::chrono::milliseconds(value->as_int64());
                }
                route_call("web", std::move(package), std::move(target), {},
                    std::move(arguments), true, timeout,
                    [complete](CallResult result) mutable {
                        try {
                            if (result.error) {
                                complete(boost::json::object{
                                    { "ok", false },
                                    { "error", boost::json::object{
                                        { "code", to_string(result.error->code) },
                                        { "message", result.error->message },
                                        { "remoteName", result.error->remote_name },
                                        { "runtimeState",
                                          result.error->runtime_state } } },
                                });
                                return;
                            }
                            complete(boost::json::object{
                                { "ok", true },
                                { "value", parse_json(
                                    result.json.value_or("null")) },
                            });
                        }
                        catch (const std::exception& error) {
                            complete(RpcFailure{ "bad_request", error.what() });
                        }
                    });
                return;
            }
            throw RpcError(
                "method_not_found", "unknown RPC method: " + method);
        }
        catch (const RpcError& error) {
            complete(RpcFailure{ std::string(error.code()), error.what() });
        }
        catch (const std::exception& error) {
            complete(RpcFailure{ "bad_request", error.what() });
        }
        catch (...) {
            complete(RpcFailure{ "internal_error", "RPC handling failed" });
        }
    }

    boost::json::value Core::handle_rpc(const boost::json::value& request) {
        auto promise = std::make_shared<std::promise<RpcOutcome>>();
        auto future = promise->get_future();
        async_handle_rpc(request, [promise](RpcOutcome outcome) mutable {
            promise->set_value(std::move(outcome));
        });
        auto outcome = future.get();
        if (auto* failure = std::get_if<RpcFailure>(&outcome)) {
            if (failure->code == "bad_request")
                throw std::runtime_error(failure->message);
            throw RpcError(failure->code, failure->message);
        }
        return std::get<boost::json::value>(std::move(outcome));
    }

} // namespace circud
