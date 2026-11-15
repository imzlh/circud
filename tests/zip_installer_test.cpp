#include "test.hpp"

#include <circud/zip_installer.hpp>

#include <minizip/zip.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("circud-test-" + std::to_string(
                                     std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

void create_zip(const std::filesystem::path& path,
                const std::vector<std::pair<std::string, std::string>>& files) {
    zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create test ZIP");
    for (const auto& [name, contents] : files) {
        zip_fileinfo info{};
        if (zipOpenNewFileInZip(archive, name.c_str(), &info, nullptr, 0,
                                nullptr, 0, nullptr, Z_DEFLATED,
                                Z_DEFAULT_COMPRESSION) != ZIP_OK ||
            zipWriteInFileInZip(archive, contents.data(), contents.size()) !=
                ZIP_OK ||
            zipCloseFileInZip(archive) != ZIP_OK) {
            zipClose(archive, nullptr);
            throw std::runtime_error("cannot write test ZIP");
        }
    }
    zipClose(archive, nullptr);
}

} // namespace

CIRCUD_TEST(zip_installs_package) {
    TempDir temp;
    auto archive = temp.path / "demo.zip";
    create_zip(archive, {
        {"meta.json", R"({"schema":1,"id":"io.example.demo","name":"demo","version":"1.0.0"})"},
        {"management.js", "export const ok = true;"},
    });
    circud::ZipInstaller installer(temp.path / "state");
    auto package = installer.install(archive);
    circud::test::require(std::filesystem::is_regular_file(
        package.version_dir / "management.js"));
}

CIRCUD_TEST(zip_same_version_uses_immutable_release_generations) {
    TempDir temp;
    const auto first_archive = temp.path / "first.zip";
    const auto second_archive = temp.path / "second.zip";
    const auto manifest =
        R"({"schema":1,"id":"io.example.demo","name":"demo","version":"1.0.0"})";
    create_zip(first_archive, {
        {"meta.json", manifest},
        {"management.js", "export const generation = 1;"},
    });
    create_zip(second_archive, {
        {"meta.json", manifest},
        {"management.js", "export const generation = 2;"},
    });
    circud::ZipInstaller installer(temp.path / "state");
    const auto first = installer.install(first_archive);
    const auto second = installer.install(second_archive);
    circud::test::require(first.version_dir != second.version_dir);
    circud::test::require(std::filesystem::exists(
        first.version_dir / "management.js"));
    circud::test::require(std::filesystem::exists(
        second.version_dir / "management.js"));
    installer.discard_release(first);
    circud::test::require(!std::filesystem::exists(first.version_dir));
    circud::test::require(std::filesystem::exists(second.version_dir));
}

CIRCUD_TEST(zip_cleanup_removes_orphans_but_keeps_active_release) {
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    create_zip(archive, {
        {"meta.json", R"({"schema":1,"id":"io.example.demo","name":"demo","version":"1.0.0"})"},
        {"management.js", "export const ok = true;"},
    });
    circud::ZipInstaller installer(temp.path / "state");
    const auto active = installer.install(archive);
    const auto orphan = active.version_dir.parent_path() / "0.9.0-orphan";
    const auto staging = temp.path / "state/staging/install-orphan";
    const auto outside = temp.path / "outside";
    const auto linked = active.version_dir.parent_path() / "0.8.0-linked";
    std::filesystem::create_directories(orphan);
    std::filesystem::create_directories(staging);
    std::filesystem::create_directories(outside);
    std::ofstream(outside / "keep.txt") << "keep";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, linked, link_error);
    installer.cleanup_orphans({active});
    circud::test::require(std::filesystem::exists(active.version_dir));
    circud::test::require(!std::filesystem::exists(orphan));
    circud::test::require(!std::filesystem::exists(staging));
    circud::test::require(std::filesystem::exists(outside / "keep.txt"));
    if (!link_error)
        circud::test::require(std::filesystem::is_symlink(linked),
                              "orphan cleanup followed release symlink");
}

CIRCUD_TEST(zip_cleanup_recovers_interrupted_remove_before_publish) {
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    create_zip(archive, {
        {"meta.json", R"({"schema":1,"id":"io.example.demo","name":"demo","version":"1.0.0"})"},
        {"management.js", "export const ok = true;"},
    });
    circud::ZipInstaller installer(temp.path / "state");
    const auto active = installer.install(archive);
    const auto package_root = active.version_dir.parent_path();
    const auto transaction = temp.path / "state/trash/io.example.demo";
    std::filesystem::create_directories(transaction);
    std::filesystem::rename(package_root, transaction / "package");
    installer.cleanup_orphans({active});
    circud::test::require(std::filesystem::exists(active.version_dir),
                          "active release was not restored from trash");
    circud::test::require(!std::filesystem::exists(transaction));
}

CIRCUD_TEST(zip_cleanup_finishes_interrupted_remove_after_publish) {
    TempDir temp;
    const auto transaction = temp.path / "state/trash/io.example.demo";
    std::filesystem::create_directories(transaction / "package/1.0.0-old");
    std::filesystem::create_directories(transaction / "data");
    std::ofstream(transaction / "data/state.txt") << "old";
    circud::ZipInstaller installer(temp.path / "state");
    installer.cleanup_orphans({});
    circud::test::require(!std::filesystem::exists(transaction),
                          "published removal trash was not reclaimed");
}

CIRCUD_TEST(zip_remove_rolls_back_files_when_publish_fails) {
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    create_zip(archive, {
        {"meta.json", R"({"schema":1,"id":"io.example.demo","name":"demo","version":"1.0.0"})"},
        {"management.js", "export const ok = true;"},
    });
    circud::ZipInstaller installer(temp.path / "state");
    const auto package = installer.install(archive);
    const auto marker = package.data_dir / "state.txt";
    std::ofstream(marker) << "keep";
    bool rejected = false;
    try {
        installer.remove(package.manifest.id, true, [] {
            throw std::runtime_error("simulated database failure");
        });
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected);
    circud::test::require(std::filesystem::exists(package.version_dir),
                          "package files were not rolled back");
    circud::test::require(std::filesystem::exists(marker),
                          "package data was not rolled back");
    const auto transaction =
        temp.path / "state/trash/io.example.demo";
    circud::test::require(!std::filesystem::exists(transaction),
                          "successful rollback left a trash transaction");
}

CIRCUD_TEST(zip_rejects_traversal) {
    TempDir temp;
    auto archive = temp.path / "bad.zip";
    create_zip(archive, {{"../escape", "bad"}});
    bool rejected = false;
    try {
        circud::ZipInstaller(temp.path / "state").install(archive);
    } catch (...) { rejected = true; }
    circud::test::require(rejected);
    circud::test::require(!std::filesystem::exists(temp.path / "escape"));
}

CIRCUD_TEST(zip_rejects_long_names) {
    TempDir temp;
    auto archive = temp.path / "long.zip";
    create_zip(archive, {{std::string(4097, 'a'), "bad"}});
    bool rejected = false;
    try {
        circud::ZipInstaller(temp.path / "state").install(archive);
    } catch (...) { rejected = true; }
    circud::test::require(rejected);
}

CIRCUD_TEST(zip_rejects_file_directory_destination_collision) {
    TempDir temp;
    const auto archive = temp.path / "collision.zip";
    create_zip(archive, {{"same/", ""}, {"same", "file"}});
    bool rejected = false;
    try {
        (void)circud::ZipInstaller(temp.path / "state").install(archive);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected,
                          "file/directory destination collision was accepted");
}

CIRCUD_TEST(zip_rejects_oversized_manifest) {
    TempDir temp;
    const auto archive = temp.path / "large-manifest.zip";
    create_zip(archive, {
        {"meta.json", std::string(64 * 1024 + 1, ' ')},
        {"management.js", "export const ok = true;"},
    });
    bool rejected = false;
    try {
        (void)circud::ZipInstaller(temp.path / "state").install(archive);
    } catch (const std::exception&) {
        rejected = true;
    }
    circud::test::require(rejected, "oversized manifest was accepted");
}
