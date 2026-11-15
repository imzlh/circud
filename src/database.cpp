#include <circud/database.hpp>

#include "json.hpp"

#include <sqlite3.h>

#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace circud {
    namespace {

        constexpr std::size_t kMaxCacheBytes = 32 * 1024 * 1024;
        constexpr std::size_t kMaxStoredPackageIdBytes = 253;
        constexpr std::size_t kMaxStoredManifestBytes = 64 * 1024;
        constexpr std::size_t kMaxStoredPathBytes = 16 * 1024;
        constexpr std::size_t kMaxStoredStatusBytes = 64 * 1024;

        void check_sqlite(int result, sqlite3* db, std::string_view operation) {
            if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
                throw std::runtime_error(std::string(operation) + ": " +
                    sqlite3_errmsg(db));
            }
        }

        void bind_text(sqlite3_stmt* statement, int index,
            std::string_view value, sqlite3* db, std::string_view operation) {
            if (value.size() > static_cast<std::size_t>(
                    std::numeric_limits<int>::max()))
                throw std::runtime_error(std::string(operation) +
                    ": value is too large");
            check_sqlite(sqlite3_bind_text(statement, index, value.data(),
                static_cast<int>(value.size()), SQLITE_TRANSIENT), db, operation);
        }

        void bind_int(sqlite3_stmt* statement, int index, int value,
            sqlite3* db, std::string_view operation) {
            check_sqlite(sqlite3_bind_int(statement, index, value), db, operation);
        }

        void bind_int64(sqlite3_stmt* statement, int index, sqlite3_int64 value,
            sqlite3* db, std::string_view operation) {
            check_sqlite(sqlite3_bind_int64(statement, index, value), db,
                operation);
        }

        std::string column_text(sqlite3_stmt* statement, int column,
            std::size_t maximum, std::string_view field) {
            if (sqlite3_column_type(statement, column) == SQLITE_NULL)
                throw std::runtime_error("database " + std::string(field) +
                    " is null");
            const auto bytes = sqlite3_column_bytes(statement, column);
            if (bytes < 0 || static_cast<std::size_t>(bytes) > maximum)
                throw std::runtime_error("database " + std::string(field) +
                    " exceeds its limit");
            const auto* text = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, column));
            if (!text && bytes != 0)
                throw std::runtime_error("cannot read database " +
                    std::string(field));
            return std::string(text ? text : "", static_cast<std::size_t>(bytes));
        }

        std::size_t checked_add(std::size_t left, std::size_t right,
            std::string_view what) {
            if (right > kMaxCacheBytes || left > kMaxCacheBytes - right)
                throw std::runtime_error(std::string(what) +
                    " exceeds the database cache limit");
            return left + right;
        }

        std::size_t cache_cost_add(std::size_t left,
            std::size_t right) noexcept {
            if (left > kMaxCacheBytes || right > kMaxCacheBytes - left)
                return kMaxCacheBytes + 1;
            return left + right;
        }

        std::size_t cache_cost_multiply(std::size_t count,
            std::size_t unit) noexcept {
            if (unit != 0 && count > kMaxCacheBytes / unit)
                return kMaxCacheBytes + 1;
            return count * unit;
        }

        std::size_t package_cache_bytes(const StoredPackage& package) noexcept {
            // Include the material fixed/container costs so a manifest with
            // many empty permission entries cannot evade the text budget.
            std::size_t result = sizeof(StoredPackage) + 128;
            const auto add = [&result](std::size_t bytes) {
                result = cache_cost_add(result, bytes);
            };
            add(package.manifest.id.size() * 2);
            add(package.manifest.name.size());
            add(package.manifest.version.size());
            add(package.manifest.entry.native().size());
            add(package.manifest.endpoint_prefix.size());
            add(cache_cost_multiply(package.manifest.permissions.read.size(),
                sizeof(std::string)));
            add(cache_cost_multiply(package.manifest.permissions.write.size(),
                sizeof(std::string)));
            add(cache_cost_multiply(package.manifest.permissions.env.size(),
                sizeof(std::string)));
            for (const auto& value : package.manifest.permissions.read)
                add(value.size());
            for (const auto& value : package.manifest.permissions.write)
                add(value.size());
            for (const auto& value : package.manifest.permissions.env)
                add(value.size());
            add(package.version_dir.native().size());
            add(package.data_dir.native().size());
            return result;
        }

        std::size_t snapshot_cache_bytes(std::string_view id,
            const StatusSnapshot& snapshot) noexcept {
            std::size_t result = sizeof(StatusSnapshot) + 96;
            result = cache_cost_add(result, id.size());
            if (snapshot.value) {
                result = cache_cost_add(result, snapshot.value->state.size());
                result = cache_cost_add(result, snapshot.value->message.size());
            }
            result = cache_cost_add(result, snapshot.error.size());
            return result;
        }

        void validate_package_memory(const StoredPackage& package) {
            if (package.manifest.id.empty() ||
                package.manifest.id.size() > kMaxStoredPackageIdBytes)
                throw std::runtime_error("stored package id is invalid");
            if (package_cache_bytes(package) > kMaxCacheBytes)
                throw std::runtime_error(
                    "package exceeds the database cache limit");
        }

        void validate_package_storage(const StoredPackage& package,
            std::string_view manifest, std::string_view version_dir,
            std::string_view data_dir) {
            validate_package_memory(package);
            if (manifest.size() > kMaxStoredManifestBytes)
                throw std::runtime_error("manifest exceeds 64 KiB");
            if (version_dir.size() > kMaxStoredPathBytes ||
                data_dir.size() > kMaxStoredPathBytes)
                throw std::runtime_error("stored package path exceeds 16 KiB");
        }

        void validate_snapshot_storage(const StatusSnapshot& snapshot) {
            std::size_t bytes = snapshot.error.size();
            if (snapshot.value) {
                if (snapshot.value->state.size() > kMaxStoredStatusBytes ||
                    bytes > kMaxStoredStatusBytes -
                        snapshot.value->state.size())
                    throw std::runtime_error("status snapshot exceeds 64 KiB");
                bytes += snapshot.value->state.size();
                if (snapshot.value->message.size() > kMaxStoredStatusBytes ||
                    bytes > kMaxStoredStatusBytes -
                        snapshot.value->message.size())
                    throw std::runtime_error("status snapshot exceeds 64 KiB");
                bytes += snapshot.value->message.size();
            }
            if (bytes > kMaxStoredStatusBytes)
                throw std::runtime_error("status snapshot exceeds 64 KiB");
        }

        struct Statement {
            sqlite3_stmt* value = nullptr;
            ~Statement() { sqlite3_finalize(value); }
        };

        std::int64_t millis(std::chrono::system_clock::time_point value) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                value.time_since_epoch())
                .count();
        }

        std::chrono::system_clock::time_point from_millis(std::int64_t value) {
            return std::chrono::system_clock::time_point(
                std::chrono::milliseconds(value));
        }

    } // namespace

    Database::Database(const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        if (sqlite3_open_v2(path.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
            std::string message = db_ ? sqlite3_errmsg(db_) : "open failed";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("open database: " + message);
        }
        try {
            check_sqlite(sqlite3_busy_timeout(db_, 5000), db_,
                "set database busy timeout");
            migrate();
            load_cache();
        }
        catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    Database::~Database() {
        if (db_) sqlite3_close(db_);
    }

    void Database::exec(std::string_view sql) const {
        char* error = nullptr;
        if (sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &error) !=
            SQLITE_OK) {
            std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("database: " + message);
        }
    }

    void Database::migrate() {
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA foreign_keys=ON");
        exec("CREATE TABLE IF NOT EXISTS packages("
            "id TEXT PRIMARY KEY, manifest TEXT NOT NULL, enabled INTEGER NOT NULL,"
            "version_dir TEXT NOT NULL, data_dir TEXT NOT NULL)");
        exec("CREATE TABLE IF NOT EXISTS snapshots("
            "package_id TEXT PRIMARY KEY REFERENCES packages(id) ON DELETE CASCADE,"
            "state TEXT, message TEXT, stale INTEGER NOT NULL, error TEXT NOT NULL,"
            "collected_at INTEGER NOT NULL, last_success_at INTEGER NOT NULL,"
            "consecutive_timeouts INTEGER NOT NULL)");
    }

    void Database::load_cache() {
        PackageCache packages;
        SnapshotCache snapshots;
        std::size_t cache_bytes = 0;

        Statement package_statement;
        check_sqlite(sqlite3_prepare_v2(db_,
            "SELECT id,manifest,enabled,version_dir,data_dir FROM packages "
            "ORDER BY id LIMIT 1025",
            -1, &package_statement.value, nullptr), db_,
            "prepare package cache load");
        for (;;) {
            const auto step = sqlite3_step(package_statement.value);
            if (step == SQLITE_DONE) break;
            check_sqlite(step, db_, "load package cache");
            if (packages.size() == kMaxStoredPackages)
                throw std::runtime_error(
                    "database contains more than 1024 packages");

            const auto id = column_text(package_statement.value, 0,
                kMaxStoredPackageIdBytes, "package id");
            const auto manifest_json = column_text(package_statement.value, 1,
                kMaxStoredManifestBytes, "package manifest");
            StoredPackage package;
            package.manifest = parse_manifest(manifest_json);
            if (package.manifest.id != id)
                throw std::runtime_error(
                    "database package id does not match its manifest");
            package.enabled = sqlite3_column_int(package_statement.value, 2) != 0;
            package.version_dir = column_text(package_statement.value, 3,
                kMaxStoredPathBytes, "package version path");
            package.data_dir = column_text(package_statement.value, 4,
                kMaxStoredPathBytes, "package data path");
            validate_package_storage(package, manifest_json,
                package.version_dir.native(), package.data_dir.native());
            cache_bytes = checked_add(cache_bytes,
                package_cache_bytes(package), "package cache");
            const auto inserted = packages.emplace(id, std::move(package));
            if (!inserted.second)
                throw std::runtime_error("duplicate package in database cache");
        }

        Statement snapshot_statement;
        check_sqlite(sqlite3_prepare_v2(db_,
            "SELECT package_id,state,message,stale,error,collected_at,"
            "last_success_at,consecutive_timeouts FROM snapshots "
            "ORDER BY package_id LIMIT 1025",
            -1, &snapshot_statement.value, nullptr), db_,
            "prepare snapshot cache load");
        for (;;) {
            const auto step = sqlite3_step(snapshot_statement.value);
            if (step == SQLITE_DONE) break;
            check_sqlite(step, db_, "load snapshot cache");
            if (snapshots.size() == kMaxStoredPackages)
                throw std::runtime_error(
                    "database contains more than 1024 snapshots");

            const auto id = column_text(snapshot_statement.value, 0,
                kMaxStoredPackageIdBytes, "snapshot package id");
            if (!packages.contains(id))
                throw std::runtime_error(
                    "database contains an orphan status snapshot");
            StatusSnapshot snapshot;
            const auto state_null = sqlite3_column_type(
                snapshot_statement.value, 1) == SQLITE_NULL;
            const auto message_null = sqlite3_column_type(
                snapshot_statement.value, 2) == SQLITE_NULL;
            if (state_null != message_null)
                throw std::runtime_error(
                    "database status value is incomplete");
            if (!state_null) {
                snapshot.value = StatusValue{
                    column_text(snapshot_statement.value, 1,
                        kMaxStoredStatusBytes, "snapshot state"),
                    column_text(snapshot_statement.value, 2,
                        kMaxStoredStatusBytes, "snapshot message") };
            }
            snapshot.stale = sqlite3_column_int(snapshot_statement.value, 3) != 0;
            snapshot.error = column_text(snapshot_statement.value, 4,
                kMaxStoredStatusBytes, "snapshot error");
            snapshot.collected_at = from_millis(
                sqlite3_column_int64(snapshot_statement.value, 5));
            snapshot.last_success_at = from_millis(
                sqlite3_column_int64(snapshot_statement.value, 6));
            const auto timeouts = sqlite3_column_int64(
                snapshot_statement.value, 7);
            if (timeouts < 0 || timeouts >
                static_cast<sqlite3_int64>(
                    std::numeric_limits<unsigned>::max()))
                throw std::runtime_error(
                    "database snapshot timeout count is invalid");
            snapshot.consecutive_timeouts = static_cast<unsigned>(timeouts);
            validate_snapshot_storage(snapshot);
            cache_bytes = checked_add(cache_bytes,
                snapshot_cache_bytes(id, snapshot), "snapshot cache");
            snapshots.emplace(id, std::move(snapshot));
        }

        packages_.swap(packages);
        snapshots_.swap(snapshots);
        cache_bytes_ = cache_bytes;
    }

    void Database::upsert_package(const StoredPackage& package) {
        validate_package_memory(package);
        const auto manifest = serialize_json(manifest_to_json(package.manifest));
        const auto version = package.version_dir.string();
        const auto data = package.data_dir.string();
        validate_package_storage(package, manifest, version, data);

        PackageCache pending;
        pending.emplace(package.manifest.id, package);
        const auto new_bytes = package_cache_bytes(package);

        std::lock_guard write_lock(write_mutex_);
        std::size_t published_bytes = 0;
        bool release_changed = false;
        {
            std::shared_lock cache_lock(cache_mutex_);
            const auto current = packages_.find(package.manifest.id);
            if (current == packages_.end() &&
                packages_.size() == kMaxStoredPackages)
                throw std::runtime_error(
                    "database cannot store more than 1024 packages");
            const auto old_bytes = current == packages_.end()
                ? 0 : package_cache_bytes(current->second);
            published_bytes = cache_bytes_ - old_bytes;
            release_changed = current != packages_.end() &&
                current->second.version_dir != package.version_dir;
            if (release_changed) {
                if (const auto snapshot = snapshots_.find(package.manifest.id);
                    snapshot != snapshots_.end())
                    published_bytes -= snapshot_cache_bytes(
                        snapshot->first, snapshot->second);
            }
            published_bytes = checked_add(published_bytes, new_bytes,
                "package cache");
        }

        exec("BEGIN IMMEDIATE");
        try {
            Statement statement;
            check_sqlite(sqlite3_prepare_v2(
                db_,
                "INSERT INTO packages VALUES(?,?,?,?,?) ON CONFLICT(id) "
                "DO UPDATE SET manifest=excluded.manifest, enabled=excluded.enabled,"
                "version_dir=excluded.version_dir, data_dir=excluded.data_dir",
                -1, &statement.value, nullptr),
                db_, "prepare upsert package");
            bind_text(statement.value, 1, package.manifest.id, db_,
                "bind package id");
            bind_text(statement.value, 2, manifest, db_,
                "bind package manifest");
            bind_int(statement.value, 3, package.enabled ? 1 : 0, db_,
                "bind package enabled state");
            bind_text(statement.value, 4, version, db_,
                "bind package version path");
            bind_text(statement.value, 5, data, db_,
                "bind package data path");
            check_sqlite(sqlite3_step(statement.value), db_,
                "upsert package");

            if (release_changed) {
                Statement clear;
                check_sqlite(sqlite3_prepare_v2(db_,
                    "DELETE FROM snapshots WHERE package_id=?", -1,
                    &clear.value, nullptr), db_,
                    "prepare clear replaced package snapshot");
                bind_text(clear.value, 1, package.manifest.id, db_,
                    "bind replaced package id");
                check_sqlite(sqlite3_step(clear.value), db_,
                    "clear replaced package snapshot");
            }
            exec("COMMIT");
        }
        catch (...) {
            try { exec("ROLLBACK"); }
            catch (...) {}
            throw;
        }

        std::unique_lock cache_lock(cache_mutex_);
        packages_.erase(package.manifest.id);
        packages_.insert(pending.extract(pending.begin()));
        if (release_changed) snapshots_.erase(package.manifest.id);
        cache_bytes_ = published_bytes;
    }

    void Database::set_enabled(std::string_view id, bool enabled) {
        if (id.size() > kMaxStoredPackageIdBytes)
            throw std::runtime_error("package not found");
        std::lock_guard write_lock(write_mutex_);
        PackageCache pending;
        std::size_t published_bytes = 0;
        {
            std::shared_lock cache_lock(cache_mutex_);
            const auto current = packages_.find(id);
            if (current == packages_.end())
                throw std::runtime_error("package not found");
            auto replacement = current->second;
            replacement.enabled = enabled;
            pending.emplace(replacement.manifest.id, std::move(replacement));
            published_bytes = cache_bytes_;
        }

        Statement statement;
        check_sqlite(sqlite3_prepare_v2(
            db_, "UPDATE packages SET enabled=? WHERE id=?", -1,
            &statement.value, nullptr),
            db_, "prepare set enabled");
        bind_int(statement.value, 1, enabled ? 1 : 0, db_,
            "bind package enabled state");
        bind_text(statement.value, 2, id, db_, "bind package id");
        check_sqlite(sqlite3_step(statement.value), db_, "set enabled");
        if (sqlite3_changes(db_) == 0) throw std::runtime_error("package not found");

        std::unique_lock cache_lock(cache_mutex_);
        packages_.erase(packages_.find(id));
        packages_.insert(pending.extract(pending.begin()));
        cache_bytes_ = published_bytes;
    }

    void Database::remove_package(std::string_view id) {
        if (id.size() > kMaxStoredPackageIdBytes)
            throw std::runtime_error("package id is too long");
        std::lock_guard write_lock(write_mutex_);
        std::size_t published_bytes = 0;
        bool cached = false;
        {
            std::shared_lock cache_lock(cache_mutex_);
            published_bytes = cache_bytes_;
            if (const auto package = packages_.find(id);
                package != packages_.end()) {
                published_bytes -= package_cache_bytes(package->second);
                cached = true;
            }
            if (const auto snapshot = snapshots_.find(id);
                snapshot != snapshots_.end())
                published_bytes -= snapshot_cache_bytes(
                    snapshot->first, snapshot->second);
        }

        Statement statement;
        check_sqlite(sqlite3_prepare_v2(db_, "DELETE FROM packages WHERE id=?", -1,
            &statement.value, nullptr),
            db_, "prepare remove package");
        bind_text(statement.value, 1, id, db_, "bind package id");
        check_sqlite(sqlite3_step(statement.value), db_, "remove package");
        if (cached && sqlite3_changes(db_) == 0)
            throw std::runtime_error(
                "package cache and database are inconsistent");

        std::unique_lock cache_lock(cache_mutex_);
        if (const auto package = packages_.find(id); package != packages_.end())
            packages_.erase(package);
        if (const auto snapshot = snapshots_.find(id);
            snapshot != snapshots_.end())
            snapshots_.erase(snapshot);
        cache_bytes_ = published_bytes;
    }

    std::vector<StoredPackage> Database::packages() const {
        std::shared_lock lock(cache_mutex_);
        std::vector<StoredPackage> result;
        result.reserve(packages_.size());
        for (const auto& [id, package] : packages_)
            result.push_back(package);
        return result;
    }

    std::optional<StoredPackage> Database::package(std::string_view id) const {
        std::shared_lock lock(cache_mutex_);
        const auto found = packages_.find(id);
        return found == packages_.end()
            ? std::nullopt : std::optional<StoredPackage>(found->second);
    }

    bool Database::save_snapshot(std::string_view id,
        const std::filesystem::path& expected_version_dir,
        const StatusSnapshot& snapshot) {
        validate_snapshot_storage(snapshot);
        const auto expected_version = expected_version_dir.string();
        if (expected_version.size() > kMaxStoredPathBytes)
            throw std::runtime_error(
                "expected package version path exceeds 16 KiB");
        std::lock_guard write_lock(write_mutex_);
        {
            std::shared_lock cache_lock(cache_mutex_);
            // Preserve the public contract for removed packages and reject an
            // old runtime's completion after the same id was reinstalled.
            const auto package = packages_.find(id);
            if (package == packages_.end() ||
                package->second.version_dir != expected_version_dir)
                return false;
        }
        SnapshotCache pending;
        pending.emplace(std::string(id), snapshot);
        const auto new_bytes = snapshot_cache_bytes(id, snapshot);

        std::size_t published_bytes = 0;
        {
            std::shared_lock cache_lock(cache_mutex_);
            const auto current = snapshots_.find(id);
            const auto old_bytes = current == snapshots_.end()
                ? 0 : snapshot_cache_bytes(current->first, current->second);
            published_bytes = cache_bytes_ - old_bytes;
            published_bytes = checked_add(published_bytes, new_bytes,
                "snapshot cache");
        }

        Statement statement;
        check_sqlite(sqlite3_prepare_v2(
            db_, "INSERT INTO snapshots(package_id,state,message,stale,error,"
            "collected_at,last_success_at,consecutive_timeouts) "
            "SELECT ?,?,?,?,?,?,?,? WHERE EXISTS("
            "SELECT 1 FROM packages WHERE id=? AND version_dir=?) "
            "ON CONFLICT(package_id) DO UPDATE SET state=excluded.state,"
            "message=excluded.message,stale=excluded.stale,error=excluded.error,"
            "collected_at=excluded.collected_at,last_success_at=excluded.last_success_at,"
            "consecutive_timeouts=excluded.consecutive_timeouts",
            -1, &statement.value, nullptr),
            db_, "prepare save snapshot");
        bind_text(statement.value, 1, id, db_, "bind snapshot package id");
        if (snapshot.value) {
            bind_text(statement.value, 2, snapshot.value->state, db_,
                "bind snapshot state");
            bind_text(statement.value, 3, snapshot.value->message, db_,
                "bind snapshot message");
        }
        else {
            check_sqlite(sqlite3_bind_null(statement.value, 2), db_,
                "bind null snapshot state");
            check_sqlite(sqlite3_bind_null(statement.value, 3), db_,
                "bind null snapshot message");
        }
        bind_int(statement.value, 4, snapshot.stale ? 1 : 0, db_,
            "bind snapshot stale state");
        bind_text(statement.value, 5, snapshot.error, db_,
            "bind snapshot error");
        bind_int64(statement.value, 6, millis(snapshot.collected_at), db_,
            "bind snapshot collection time");
        bind_int64(statement.value, 7, millis(snapshot.last_success_at), db_,
            "bind snapshot success time");
        bind_int64(statement.value, 8,
            static_cast<sqlite3_int64>(snapshot.consecutive_timeouts), db_,
            "bind snapshot timeout count");
        bind_text(statement.value, 9, id, db_, "bind snapshot package id");
        bind_text(statement.value, 10, expected_version, db_,
            "bind snapshot package version path");
        check_sqlite(sqlite3_step(statement.value), db_, "save snapshot");
        if (sqlite3_changes(db_) == 0) return false;

        std::unique_lock cache_lock(cache_mutex_);
        if (const auto current = snapshots_.find(id);
            current != snapshots_.end())
            snapshots_.erase(current);
        snapshots_.insert(pending.extract(pending.begin()));
        cache_bytes_ = published_bytes;
        return true;
    }

    std::optional<StatusSnapshot> Database::snapshot(std::string_view id) const {
        std::shared_lock lock(cache_mutex_);
        const auto found = snapshots_.find(id);
        return found == snapshots_.end()
            ? std::nullopt : std::optional<StatusSnapshot>(found->second);
    }

} // namespace circud
