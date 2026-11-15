#include <circud/http_background.hpp>

#include <algorithm>
#include <utility>

namespace circud {

    std::shared_ptr<HttpBackground> HttpBackground::create(
        std::size_t workers, Task stopped) {
        auto result = std::shared_ptr<HttpBackground>(
            new HttpBackground(workers, std::move(stopped)));
        result->start();
        return result;
    }

    HttpBackground::HttpBackground(std::size_t workers, Task stopped)
        : worker_count_(std::max<std::size_t>(1, workers)),
          stopped_(std::move(stopped)) {}

    HttpBackground::~HttpBackground() {
        stop();
        join();
    }

    void HttpBackground::start() {
        threads_.reserve(worker_count_);
        try {
            for (std::size_t index = 0; index < worker_count_; ++index)
                threads_.emplace_back([this] { run(); });
        }
        catch (...) {
            stop();
            join();
            throw;
        }
    }

    bool HttpBackground::post(std::size_t bytes, Task work, Task cancel) {
        if (!work || bytes > max_queued_bytes) return false;
        std::lock_guard lock(mutex_);
        if (!accepting_ || queue_.size() + running_ >= max_queued_tasks ||
            bytes > max_queued_bytes - queued_bytes_)
            return false;
        queued_bytes_ += bytes;
        queue_.push_back(Entry{
            bytes, std::move(work), std::move(cancel), false });
        changed_.notify_one();
        return true;
    }

    void HttpBackground::stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!accepting_) return;
            accepting_ = false;
            // Workers drain cancellation callbacks instead of running queued
            // filesystem work. Destruction of task-owned file descriptors and
            // streams consequently also stays off the network/control thread.
            for (auto& entry : queue_) entry.cancelled = true;
        }
        changed_.notify_all();
    }

    bool HttpBackground::accepting() const noexcept {
        std::lock_guard lock(mutex_);
        return accepting_;
    }

    bool HttpBackground::join_ready() const noexcept {
        std::lock_guard lock(mutex_);
        return !accepting_ && exited_ == worker_count_;
    }

    void HttpBackground::join() noexcept {
        std::lock_guard join_lock(join_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (joined_) return;
            joined_ = true;
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
    }

    void HttpBackground::run() noexcept {
        for (;;) {
            Entry entry;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this] {
                    return !accepting_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (!accepting_) break;
                    continue;
                }
                entry = std::move(queue_.front());
                queue_.pop_front();
                ++running_;
            }
            try {
                auto& task = entry.cancelled ? entry.cancel : entry.work;
                if (task) task();
            }
            catch (...) {}
            {
                std::lock_guard lock(mutex_);
                if (running_ != 0) --running_;
                queued_bytes_ = entry.bytes > queued_bytes_
                    ? 0 : queued_bytes_ - entry.bytes;
            }
        }

        Task stopped;
        {
            std::lock_guard lock(mutex_);
            ++exited_;
            if (exited_ == worker_count_) stopped = stopped_;
        }
        if (stopped) {
            try { stopped(); }
            catch (...) {}
        }
    }

} // namespace circud
