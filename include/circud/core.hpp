#pragma once

#include <circud/config.hpp>
#include <circud/database.hpp>
#include <circud/event_journal.hpp>
#include <circud/package_runtime.hpp>
#include <circud/state_lock.hpp>
#include <circud/zip_installer.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json/value.hpp>

#include <atomic>
#include <deque>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>

namespace circud {

    class EndpointGateway;

    struct RpcFailure {
        std::string code;
        std::string message;
    };

    using RpcOutcome = std::variant<boost::json::value, RpcFailure>;
    using LifecycleCompletion =
        std::function<void(std::optional<std::string>)>;

    class Core {
    public:
        explicit Core(CoreConfig config);
        ~Core();
        Core(const Core&) = delete;
        Core& operator=(const Core&) = delete;

        void start();
        void stop();
        boost::asio::io_context& io() noexcept { return io_; }
        const CoreConfig& config() const noexcept { return config_; }
        EndpointGateway& endpoint_gateway() noexcept;
        const EndpointGateway& endpoint_gateway() const noexcept;

        StoredPackage install(const std::filesystem::path& archive);
        void remove(std::string_view id, bool purge_data);
        void async_start_package(std::string id,
            LifecycleCompletion completion = {});
        void async_stop_package(std::string id,
            LifecycleCompletion completion = {});
        void async_reload_package(std::string id,
            LifecycleCompletion completion = {});
        void async_remove_package(std::string id, bool purge_data,
            LifecycleCompletion completion = {});
        // Future adapters are for CLI/background/test threads only. Waiting on
        // one from io()'s control thread deadlocks by definition.
        std::future<void> start_package(std::string id);
        std::future<void> stop_package(std::string id);
        std::future<void> reload_package(std::string id);
        std::vector<StoredPackage> packages() const;
        RuntimeState runtime_state(std::string_view id) const;
        std::string runtime_failure(std::string_view id) const;
        std::optional<StatusSnapshot> status(std::string_view id) const;

        std::future<CallResult> call(std::string source, std::string package_id,
            std::string target, std::string json_arguments,
            std::chrono::milliseconds timeout);
        std::future<StatusSnapshot> refresh_status(std::string package_id);
        EventPage management_events(const EventQuery& query) const;
        void async_handle_rpc(boost::json::value request,
            std::function<void(RpcOutcome)> completion);
        boost::json::value handle_rpc(const boost::json::value& request);

    private:
        friend class EndpointGateway;
        struct PendingCall;
        struct PendingStatusPersistence {
            std::filesystem::path version_dir;
            StatusSnapshot snapshot;
        };
        class RuntimeReaper;
        class BlockingWorker;
        // quiesce is an internal install barrier: it retires the exact current
        // generation without changing the persisted enabled intent.
        enum class LifecycleAction { start, stop, reload, remove, quiesce };
        struct LifecycleRequest {
            LifecycleAction action = LifecycleAction::start;
            LifecycleCompletion completion;
            bool purge_data = false;
            std::optional<StoredPackage> previous;
        };
        struct LifecycleLane {
            std::deque<LifecycleRequest> queued;
            std::optional<LifecycleRequest> active;
            std::shared_ptr<boost::asio::steady_timer> timer;
            std::uint64_t token = 0;
            bool retirement_started = false;
        };
        void restore_packages();
        void submit_lifecycle(std::string id, LifecycleAction action,
            LifecycleCompletion completion,
            bool package_operation = false);
        void drive_lifecycle(const std::string& id);
        void persist_lifecycle_enabled(const std::string& id, bool enabled);
        void on_lifecycle_enabled(std::string id,
            std::uint64_t daemon_generation, std::uint64_t token,
            std::optional<std::string> error);
        void begin_runtime_start(const std::string& id, bool reload);
        void begin_runtime_retire(const std::string& id,
            std::string reason, bool then_start);
        void begin_package_remove(const std::string& id);
        void on_runtime_ready(std::string id, std::uint64_t daemon_generation,
            std::uint64_t token, std::shared_ptr<PackageRuntime> runtime,
            bool reload, RuntimeState state);
        void on_runtime_start_timeout(std::string id,
            std::uint64_t daemon_generation, std::uint64_t token,
            std::shared_ptr<PackageRuntime> runtime, bool reload);
        void on_runtime_stop_phase(std::string id,
            std::uint64_t daemon_generation, std::uint64_t token,
            std::shared_ptr<PackageRuntime> runtime, bool then_start,
            bool timed_out);
        void on_runtime_retired(std::string id,
            std::uint64_t daemon_generation, std::uint64_t token,
            bool then_start);
        void on_package_removed(std::string id, bool purge_data,
            std::optional<StoredPackage> previous,
            std::optional<std::string> error);
        void finish_lifecycle(const std::string& id,
            std::optional<std::string> error = std::nullopt);
        bool retire_runtime(const std::shared_ptr<PackageRuntime>& runtime,
            std::string reason, std::function<void()> joined = {});
        void stop_on_control(std::uint64_t stop_generation,
            std::function<void()> completion);
        void route_call(std::string source, std::string package_id,
            std::string target, ClonePtr arguments,
            std::optional<std::string> json_arguments, bool wants_json,
            std::chrono::milliseconds timeout,
            std::function<void(CallResult)> completion);
        void deliver_call(const std::shared_ptr<PendingCall>& call,
            CallResult result) noexcept;
        void finish_call(std::uint64_t id, CallResult result);
        void fail_calls_for_runtime(
            const std::shared_ptr<PackageRuntime>& runtime,
            std::string message);
        void arm_status_timer(std::uint64_t generation);
        void poll_statuses();
        void collect_status(std::string package_id,
            std::function<void(StatusSnapshot)> completion);
        bool queue_status_persistence(std::string package_id,
            std::filesystem::path version_dir, StatusSnapshot snapshot);
        void drain_status_persistence() noexcept;
        void deliver_status(std::string package_id,
            StatusSnapshot snapshot) noexcept;
        void record_event(EventLevel level, std::string_view category,
            std::string_view action, std::string_view message,
            std::optional<std::string_view> package_id = std::nullopt,
            std::string_view code = {}, boost::json::object details = {});
        void record_call_failure(const PendingCall& call,
            const CallResult& result);

        CoreConfig config_;
        StateLock state_lock_;
        mutable std::recursive_mutex lifecycle_mutex_;
        mutable std::mutex mutex_;
        Database database_;
        ZipInstaller installer_;
        EventJournal events_;
        boost::asio::io_context io_;
        std::optional<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>> work_;
        boost::asio::steady_timer status_timer_;
        std::unique_ptr<EndpointGateway> endpoint_gateway_;
        std::unique_ptr<RuntimeReaper> reaper_;
        std::unique_ptr<BlockingWorker> blocking_worker_;
        std::unordered_map<std::string, std::shared_ptr<PackageRuntime>> runtimes_;
        std::unordered_map<std::string, std::shared_ptr<PackageRuntime>>
            lifecycle_runtimes_;
        std::unordered_map<std::string, LifecycleLane> lifecycle_lanes_;
        std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pending_;
        std::unordered_map<std::string,
            std::vector<std::function<void(StatusSnapshot)>>>
            status_waiters_;
        std::unordered_map<std::string, StatusSnapshot> status_snapshots_;
        std::unordered_map<std::string, PendingStatusPersistence>
            dirty_status_snapshots_;
        std::size_t status_waiter_count_ = 0;
        std::atomic<std::uint64_t> next_call_id_{ 1 };
        std::atomic<std::size_t> package_operations_{ 0 };
        std::atomic<std::size_t> database_operations_{ 0 };
        std::uint64_t next_lifecycle_token_ = 0;
        std::uint64_t generation_ = 0;
        bool status_persistence_scheduled_ = false;
        bool started_ = false;
        bool stopping_ = false;
    };

} // namespace circud
