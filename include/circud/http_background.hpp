#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace circud {

    // A generation-owned, bounded executor for filesystem work initiated by
    // the HTTP listener. stop() closes admission and cancels queued work but
    // never waits for a running system call. A stopped generation can be
    // joined later after join_ready() becomes true.
    class HttpBackground final {
    public:
        using Task = std::function<void()>;

        static constexpr std::size_t max_queued_tasks = 64;
        static constexpr std::size_t max_queued_bytes = 4 * 1024 * 1024;

        static std::shared_ptr<HttpBackground> create(
            std::size_t workers, Task stopped = {});
        ~HttpBackground();

        HttpBackground(const HttpBackground&) = delete;
        HttpBackground& operator=(const HttpBackground&) = delete;

        // bytes accounts for memory retained by the queued task. A false
        // result means the generation is stopping or its task/byte budget is
        // full. cancel is invoked exactly once on a worker for work discarded
        // by stop().
        bool post(std::size_t bytes, Task work, Task cancel = {});
        void stop() noexcept;
        bool accepting() const noexcept;
        bool join_ready() const noexcept;
        void join() noexcept;

    private:
        struct Entry {
            std::size_t bytes = 0;
            Task work;
            Task cancel;
            bool cancelled = false;
        };

        HttpBackground(std::size_t workers, Task stopped);
        void start();
        void run() noexcept;

        const std::size_t worker_count_;
        Task stopped_;
        std::mutex join_mutex_;
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::deque<Entry> queue_;
        std::vector<std::thread> threads_;
        std::size_t queued_bytes_ = 0;
        std::size_t running_ = 0;
        std::size_t exited_ = 0;
        bool accepting_ = true;
        bool joined_ = false;
    };

} // namespace circud
