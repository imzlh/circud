#pragma once

#include <circud/config.hpp>
#include <circud/database.hpp>
#include <circud/package_endpoint.hpp>
#include <circud/types.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>

struct TJSRuntime;
struct JSContext;
struct uv_async_s;

namespace circud {

    struct RuntimeCall {
        std::uint64_t id = 0;
        std::string source;
        std::string target;
        ClonePtr arguments;
        std::optional<std::string> json_arguments;
        bool wants_json = false;
        std::chrono::steady_clock::time_point deadline;
        std::function<void(CallResult)> complete;
    };

    using OutboundCall = std::function<void(
        std::string package_id, std::string target, ClonePtr arguments,
        std::chrono::milliseconds timeout, std::function<void(CallResult)> complete)>;

    class PackageRuntime : public std::enable_shared_from_this<PackageRuntime> {
    public:
        PackageRuntime(StoredPackage package, OutboundCall outbound,
            std::function<void()> request_reload,
            EndpointConfig endpoint_config = {});
        ~PackageRuntime();
        PackageRuntime(const PackageRuntime&) = delete;
        PackageRuntime& operator=(const PackageRuntime&) = delete;

        using StartCompletion = std::function<void(RuntimeState)>;
        using StopCompletion = std::function<void()>;

        // Thread creation, graceful shutdown and thread reclamation are three
        // distinct phases. The control actor only uses the non-blocking first
        // three methods; join ownership is claimed once and handed to Core's
        // reaper. stop_and_join() is reserved for final daemon shutdown and
        // tests running outside the I/O/control thread.
        void async_start(StartCompletion completion);
        bool async_graceful_stop(std::string reason,
            StopCompletion completion);
        void request_shutdown(std::string reason = "shutdown") noexcept;
        bool notify_on_exit(std::function<void()> completion);
        bool exited() const noexcept { return exited_.load(); }
        bool claim_join() noexcept;
        void release_join_claim() noexcept { join_claimed_.store(false); }
        void join();
        void stop_and_join(std::string reason = "shutdown");
        bool enqueue(RuntimeCall call);
        void cancel(std::uint64_t id);
        bool open_endpoint(EndpointRequest request,
            std::shared_ptr<EndpointSink> sink);
        bool deliver_endpoint_message(EndpointMessage message);
        bool close_endpoint(EndpointClose close);
        bool post(std::function<void(JSContext*)> task);
        void mark_stuck();
        void clear_stuck();

        const StoredPackage& package() const noexcept { return package_; }
        RuntimeState state() const noexcept { return state_.load(); }
        std::string failure() const;

    private:
        static void async_callback(uv_async_s* handle) noexcept;
        void run(StartCompletion completion) noexcept;
        void teardown() noexcept;
        void fail(std::string_view message) noexcept;
        void finish_graceful_stop() noexcept;
        void signal_exited() noexcept;
        void drain();
        void fail_queued(CallErrorCode code, std::string_view message) noexcept;
        bool allowed_path(std::string_view resource, bool write) const;

        struct OpenEndpoint {
            EndpointRequest request;
            std::shared_ptr<EndpointSink> sink;
        };
        using EndpointCommand =
            std::variant<OpenEndpoint, EndpointMessage>;

        StoredPackage package_;
        OutboundCall outbound_;
        std::function<void()> request_reload_;
        std::atomic<RuntimeState> state_{ RuntimeState::disabled };
        std::atomic_bool join_claimed_{ false };
        std::atomic_bool exited_{ false };
        std::thread thread_;
        mutable std::mutex mutex_;
        std::deque<RuntimeCall> queue_;
        std::deque<EndpointCommand> endpoint_commands_;
        // Close is control traffic: it must not compete with bounded endpoint
        // data. IDs remain pending until owner-thread dispatch, so repeated
        // close requests are idempotent and cannot grow this queue. Capacity
        // covers live sessions plus opens admitted while older closes wait.
        std::deque<EndpointClose> endpoint_closes_;
        std::unordered_set<std::uint64_t> pending_endpoint_close_ids_;
        // Core has at most kMaxPendingCalls live management calls; reserve one
        // additional slot for the internal lifecycle.stop call.
        std::deque<std::uint64_t> call_cancellations_;
        std::unordered_set<std::uint64_t> pending_call_cancellation_ids_;
        std::deque<std::function<void(JSContext*)>> tasks_;
        std::size_t queue_bytes_ = 0;
        std::size_t endpoint_command_bytes_ = 0;
        EndpointConfig endpoint_config_;
        std::size_t max_endpoint_command_bytes_ = kMaxEndpointCommandBytes;
        std::size_t max_endpoint_closes_ = 1;
        std::size_t max_pending_endpoint_closes_ = 2;
        std::size_t max_call_cancellations_ = kMaxPendingCalls + 1;
        // These tasks deliver completed outbound calls back to their owning
        // runtime. A package cannot have more than kMaxPendingCalls outbound
        // calls, so the same bound admits every legitimate completion while
        // preventing a stalled owner thread from growing this queue forever.
        std::size_t max_tasks_ = kMaxPendingCalls;
        std::size_t max_lifecycle_waiters_ = kMaxPendingCalls;
        bool stopping_ = false;
        bool graceful_stop_started_ = false;
        bool graceful_stop_finished_ = false;
        std::vector<StopCompletion> graceful_stop_waiters_;
        std::vector<std::function<void()>> exit_waiters_;
        std::string failure_;
        TJSRuntime* runtime_ = nullptr;
        JSContext* context_ = nullptr;
        uv_async_s* async_ = nullptr;
    };

} // namespace circud
