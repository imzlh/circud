#include "test.hpp"

#include <circud/database.hpp>
#include <circud/state_lock.hpp>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("circud-db-test-" + std::to_string(
                                        std::chrono::steady_clock::now()
                                            .time_since_epoch()
                                            .count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

circud::StoredPackage stored_package(std::string id = "io.example.cache") {
    circud::StoredPackage package;
    package.manifest.id = std::move(id);
    package.manifest.name = "Cache test";
    package.manifest.version = "1.0.0";
    package.version_dir = "/packages/cache/1.0.0";
    package.data_dir = "/data/cache";
    return package;
}

circud::StatusSnapshot status_snapshot(std::string state = "running") {
    circud::StatusSnapshot snapshot;
    snapshot.value = circud::StatusValue{
        std::move(state), "database cache test" };
    snapshot.stale = false;
    snapshot.collected_at = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1234));
    snapshot.last_success_at = snapshot.collected_at;
    snapshot.consecutive_timeouts = 2;
    return snapshot;
}

void execute_sql(const std::filesystem::path& path, std::string_view sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database,
        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        const std::string message = database
            ? sqlite3_errmsg(database) : "open failed";
        if (database) sqlite3_close(database);
        throw std::runtime_error("open test database: " + message);
    }
    char* error = nullptr;
    const auto result = sqlite3_exec(database, std::string(sql).c_str(),
        nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error("execute test SQL: " + message);
    }
    sqlite3_close(database);
}

template <typename Function>
void require_throws(Function&& function, std::string message) {
    bool threw = false;
    try { function(); }
    catch (const std::exception&) { threw = true; }
    circud::test::require(threw, std::move(message));
}

} // namespace

CIRCUD_TEST(database_cache_persists_packages_and_snapshots) {
    TempDir temp;
    const auto path = temp.path / "circud.db";
    {
        circud::Database database(path);
        auto first = stored_package("io.example.alpha");
        auto second = stored_package("io.example.beta");
        second.manifest.name = "Beta";
        second.enabled = false;
        database.upsert_package(second);
        database.upsert_package(first);
        database.save_snapshot(first.manifest.id, first.version_dir,
            status_snapshot());

        const auto packages = database.packages();
        circud::test::require(packages.size() == 2);
        circud::test::require(
            packages[0].manifest.id == "io.example.alpha");
        circud::test::require(
            packages[1].manifest.id == "io.example.beta");
        circud::test::require(!database.package("io.example.beta")->enabled);
        circud::test::require(
            database.snapshot("io.example.alpha")->value->state == "running");

        database.set_enabled("io.example.beta", true);
        circud::test::require(database.package("io.example.beta")->enabled);
    }
    {
        circud::Database reopened(path);
        circud::test::require(reopened.packages().size() == 2);
        circud::test::require(reopened.package("io.example.beta")->enabled);
        const auto snapshot = reopened.snapshot("io.example.alpha");
        circud::test::require(snapshot.has_value());
        circud::test::require(snapshot->value->message == "database cache test");
        circud::test::require(snapshot->consecutive_timeouts == 2);
        circud::test::require(snapshot->collected_at ==
            std::chrono::system_clock::time_point(
                std::chrono::milliseconds(1234)));
    }
}

CIRCUD_TEST(database_cache_removal_evicts_status_snapshot) {
    TempDir temp;
    const auto path = temp.path / "circud.db";
    {
        circud::Database database(path);
        const auto package = stored_package();
        database.upsert_package(package);
        database.save_snapshot(package.manifest.id, package.version_dir,
            status_snapshot());
        database.remove_package(package.manifest.id);
        circud::test::require(!database.package(package.manifest.id));
        circud::test::require(!database.snapshot(package.manifest.id));
    }
    circud::Database reopened(path);
    circud::test::require(reopened.packages().empty());
    circud::test::require(!reopened.snapshot("io.example.cache"));
}

CIRCUD_TEST(database_package_release_replacement_invalidates_snapshot) {
    TempDir temp;
    const auto path = temp.path / "circud.db";
    auto first = stored_package();
    auto replacement = first;
    replacement.manifest.version = "2.0.0";
    replacement.version_dir = "/packages/cache/2.0.0";
    {
        circud::Database database(path);
        database.upsert_package(first);
        circud::test::require(database.save_snapshot(first.manifest.id,
            first.version_dir, status_snapshot("old-release")));

        database.upsert_package(replacement);
        circud::test::require(!database.snapshot(first.manifest.id),
            "replacement release retained the previous status snapshot");
        circud::test::require(!database.save_snapshot(first.manifest.id,
            first.version_dir, status_snapshot("late-old-release")),
            "old release persisted a snapshot after replacement");
        circud::test::require(database.save_snapshot(first.manifest.id,
            replacement.version_dir, status_snapshot("new-release")),
            "current release snapshot was rejected");
        circud::test::require(
            database.snapshot(first.manifest.id)->value->state ==
                "new-release");
    }

    circud::Database reopened(path);
    const auto snapshot = reopened.snapshot(first.manifest.id);
    circud::test::require(snapshot && snapshot->value &&
            snapshot->value->state == "new-release",
        "release-bound snapshot did not persist across reopen");
}

CIRCUD_TEST(database_same_release_metadata_update_preserves_snapshot) {
    TempDir temp;
    circud::Database database(temp.path / "circud.db");
    auto package = stored_package();
    database.upsert_package(package);
    database.save_snapshot(package.manifest.id, package.version_dir,
        status_snapshot("same-release"));

    package.enabled = false;
    package.manifest.name = "Updated metadata";
    database.upsert_package(package);
    const auto snapshot = database.snapshot(package.manifest.id);
    circud::test::require(snapshot && snapshot->value &&
            snapshot->value->state == "same-release",
        "same immutable release metadata update discarded its snapshot");
}

CIRCUD_TEST(database_cache_is_unchanged_when_sqlite_write_fails) {
    TempDir temp;
    const auto path = temp.path / "circud.db";
    circud::Database database(path);
    const auto original = stored_package();
    database.upsert_package(original);
    const auto original_snapshot = status_snapshot("running");
    database.save_snapshot(original.manifest.id, original.version_dir,
        original_snapshot);

    execute_sql(path,
        "CREATE TRIGGER reject_package_update BEFORE UPDATE ON packages "
        "BEGIN SELECT RAISE(ABORT,'package update rejected'); END;"
        "CREATE TRIGGER reject_package_delete BEFORE DELETE ON packages "
        "BEGIN SELECT RAISE(ABORT,'package delete rejected'); END;"
        "CREATE TRIGGER reject_snapshot_update BEFORE UPDATE ON snapshots "
        "BEGIN SELECT RAISE(ABORT,'snapshot update rejected'); END;");

    auto replacement = original;
    replacement.manifest.name = "Replacement";
    replacement.manifest.version = "2.0.0";
    replacement.enabled = false;
    require_throws([&] { database.upsert_package(replacement); },
        "failed package upsert changed no error state");
    require_throws([&] { database.set_enabled(original.manifest.id, false); },
        "failed enabled update changed no error state");
    require_throws([&] { database.remove_package(original.manifest.id); },
        "failed package removal changed no error state");
    require_throws([&] {
        database.save_snapshot(original.manifest.id, original.version_dir,
            status_snapshot("failed"));
    }, "failed snapshot update changed no error state");

    const auto cached = database.package(original.manifest.id);
    circud::test::require(cached.has_value());
    circud::test::require(cached->manifest.name == original.manifest.name);
    circud::test::require(cached->manifest.version == "1.0.0");
    circud::test::require(cached->enabled);
    const auto cached_snapshot = database.snapshot(original.manifest.id);
    circud::test::require(cached_snapshot.has_value());
    circud::test::require(cached_snapshot->value->state == "running");
}

CIRCUD_TEST(database_reads_use_the_loaded_cache) {
    TempDir temp;
    const auto path = temp.path / "circud.db";
    circud::Database database(path);
    const auto package = stored_package();
    database.upsert_package(package);
    database.save_snapshot(package.manifest.id, package.version_dir,
        status_snapshot());

    // The state-directory lock makes out-of-process writes unsupported. This
    // deliberate mutation verifies that read APIs do not prepare or step SQL.
    execute_sql(path, "DELETE FROM packages;");
    circud::test::require(database.package(package.manifest.id).has_value());
    circud::test::require(database.packages().size() == 1);
    circud::test::require(database.snapshot(package.manifest.id).has_value());
}

CIRCUD_TEST(database_cache_supports_concurrent_readers_and_writes) {
    TempDir temp;
    circud::Database database(temp.path / "circud.db");
    const auto package = stored_package();
    database.upsert_package(package);
    database.save_snapshot(package.manifest.id, package.version_dir,
        status_snapshot("state-0"));

    std::atomic_bool done = false;
    std::atomic_bool failed = false;
    std::jthread writer([&] {
        try {
            for (unsigned index = 1; index <= 100; ++index) {
                database.set_enabled(package.manifest.id, index % 2 == 0);
                database.save_snapshot(package.manifest.id,
                    package.version_dir,
                    status_snapshot("state-" + std::to_string(index % 2)));
            }
        }
        catch (...) { failed = true; }
        done = true;
    });
    std::vector<std::jthread> readers;
    for (unsigned index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            try {
                while (!done.load()) {
                    const auto one = database.package(package.manifest.id);
                    const auto all = database.packages();
                    const auto snapshot = database.snapshot(package.manifest.id);
                    if (!one || all.size() != 1 || !snapshot ||
                        one->manifest.id != package.manifest.id ||
                        !snapshot->value ||
                        (snapshot->value->state != "state-0" &&
                            snapshot->value->state != "state-1")) {
                        failed = true;
                        return;
                    }
                }
            }
            catch (...) { failed = true; }
        });
    }
    writer.join();
    for (auto& reader : readers) reader.join();
    circud::test::require(!failed,
        "concurrent database cache access observed invalid state");
    circud::test::require(database.package(package.manifest.id)->enabled);
    circud::test::require(
        database.snapshot(package.manifest.id)->value->state == "state-0");
}

CIRCUD_TEST(snapshot_for_removed_package_is_ignored) {
    TempDir temp;
    circud::Database database(temp.path / "circud.db");
    circud::StatusSnapshot snapshot;
    snapshot.error = "package not found";
    snapshot.collected_at = std::chrono::system_clock::now();
    database.save_snapshot("io.example.missing", "/packages/missing",
        snapshot);
    circud::test::require(!database.snapshot("io.example.missing"));
}

CIRCUD_TEST(state_directory_allows_only_one_owner) {
    TempDir temp;
    const auto state = temp.path / "state";
    {
        circud::StateLock first(state);
        bool rejected = false;
        try {
            circud::StateLock second(state);
        } catch (const std::exception&) {
            rejected = true;
        }
        circud::test::require(rejected,
                              "second state directory owner was accepted");
    }
    circud::StateLock after_release(state);
}

CIRCUD_TEST(state_lock_does_not_follow_symlinks) {
    TempDir temp;
    const auto state = temp.path / "state";
    const auto outside = temp.path / "outside.txt";
    std::filesystem::create_directories(state);
    {
        std::ofstream stream(outside);
        stream << "preserve";
    }
    std::error_code error;
    std::filesystem::create_symlink(outside, state / "circud.lock", error);
    if (error) return;
    bool rejected = false;
    try {
        circud::StateLock lock(state);
    } catch (const std::exception&) {
        rejected = true;
    }
    std::ifstream stream(outside);
    std::string contents;
    stream >> contents;
    circud::test::require(rejected, "state lock symlink was accepted");
    circud::test::require(contents == "preserve",
                          "state lock followed and modified symlink target");
}
