#pragma once

#include <circud/manifest.hpp>
#include <circud/types.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace circud {

    inline constexpr std::size_t kMaxStoredPackages = 1024;

    struct StoredPackage {
        PackageManifest manifest;
        bool enabled = true;
        std::filesystem::path version_dir;
        std::filesystem::path data_dir;
    };

    class Database {
    public:
        explicit Database(const std::filesystem::path& path);
        ~Database();
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        void upsert_package(const StoredPackage& package);
        void set_enabled(std::string_view id, bool enabled);
        void remove_package(std::string_view id);
        std::vector<StoredPackage> packages() const;
        std::optional<StoredPackage> package(std::string_view id) const;
        // Persist only when id still names the exact immutable release that
        // produced the snapshot. Returns false for a removed or replaced
        // release; stale worker completions are intentionally ignored.
        bool save_snapshot(std::string_view id,
            const std::filesystem::path& expected_version_dir,
            const StatusSnapshot& snapshot);
        std::optional<StatusSnapshot> snapshot(std::string_view id) const;

    private:
        using PackageCache =
            std::map<std::string, StoredPackage, std::less<>>;
        using SnapshotCache =
            std::map<std::string, StatusSnapshot, std::less<>>;

        void exec(std::string_view sql) const;
        void migrate();
        void load_cache();

        // SQLite writes stay serialized, while cache-only readers never wait
        // for SQLite I/O or its busy timeout.
        std::mutex write_mutex_;
        mutable std::shared_mutex cache_mutex_;
        PackageCache packages_;
        SnapshotCache snapshots_;
        std::size_t cache_bytes_ = 0;
        sqlite3* db_ = nullptr;
    };

} // namespace circud
