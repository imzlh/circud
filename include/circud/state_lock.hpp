#pragma once

#include <filesystem>

namespace circud {

    class StateLock {
    public:
        explicit StateLock(const std::filesystem::path& state_dir);
        ~StateLock();
        StateLock(const StateLock&) = delete;
        StateLock& operator=(const StateLock&) = delete;
        StateLock(StateLock&&) = delete;
        StateLock& operator=(StateLock&&) = delete;

    private:
        int descriptor_ = -1;
    };

} // namespace circud
