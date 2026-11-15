#include <circud/zip_installer.hpp>

#include <minizip/unzip.h>

#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_set>

namespace circud {
    namespace {

        struct Archive {
            unzFile value = nullptr;
            ~Archive() {
                if (value) unzClose(value);
            }
        };

        bool unsafe_entry(std::string_view name) {
            if (name.empty() || name.front() == '/' || name.front() == '\\' ||
                name.find('\\') != std::string_view::npos || name.find('\0') !=
                std::string_view::npos)
                return true;
            std::filesystem::path path(name);
            if (path.is_absolute()) return true;
            for (const auto& part : path)
                if (part == ".." || part == ".") return true;
            return false;
        }

        void remove_tree(const std::filesystem::path& path) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::string unique_suffix() {
            static std::atomic<std::uint64_t> sequence{ 0 };
            std::ostringstream value;
            value << std::hex
                << std::chrono::steady_clock::now().time_since_epoch().count()
                << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
            return value.str();
        }

        std::filesystem::path create_staging_directory(
            const std::filesystem::path& root) {
            std::filesystem::create_directories(root);
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                auto candidate = root / ("install-" + unique_suffix());
                std::error_code error;
                if (std::filesystem::create_directory(candidate, error))
                    return candidate;
                if (error && error != std::errc::file_exists)
                    throw std::runtime_error("cannot create ZIP staging directory");
            }
            throw std::runtime_error("cannot allocate ZIP staging directory");
        }

        bool is_owned_release(const std::filesystem::path& state_dir,
            const StoredPackage& package) {
            if (!is_reverse_domain_id(package.manifest.id)) return false;
            std::error_code error;
            const auto expected = std::filesystem::weakly_canonical(
                state_dir / "packages" / package.manifest.id, error);
            if (error) return false;
            const auto release =
                std::filesystem::weakly_canonical(package.version_dir, error);
            if (error || release.parent_path() != expected) return false;
            const auto name = release.filename().string();
            return name == package.manifest.version ||
                name.starts_with(package.manifest.version + "-");
        }

        void rename_if_present(const std::filesystem::path& source,
            const std::filesystem::path& destination, bool& moved) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(source, error);
            if (error == std::errc::no_such_file_or_directory ||
                status.type() == std::filesystem::file_type::not_found) {
                moved = false;
                return;
            }
            if (error) throw std::filesystem::filesystem_error(
                "inspect removal source", source, error);
            std::filesystem::rename(source, destination, error);
            if (error) throw std::filesystem::filesystem_error(
                "stage package removal", source, destination, error);
            moved = true;
        }

        bool restore_if_moved(const std::filesystem::path& staged,
            const std::filesystem::path& destination, bool moved) noexcept {
            if (!moved) return true;
            std::error_code error;
            if (std::filesystem::exists(destination, error) || error)
                return false;
            std::filesystem::rename(staged, destination, error);
            return !error;
        }

    } // namespace

    ZipInstaller::ZipInstaller(std::filesystem::path state_dir, ZipLimits limits)
        : state_dir_(std::move(state_dir)), limits_(limits) {}

    StoredPackage ZipInstaller::install(const std::filesystem::path& archive_path) {
        if (!std::filesystem::is_regular_file(archive_path))
            throw std::runtime_error("archive does not exist");
        if (std::filesystem::file_size(archive_path) > limits_.max_archive_bytes)
            throw std::runtime_error("archive is too large");

        Archive archive{ unzOpen64(archive_path.c_str()) };
        if (!archive.value) throw std::runtime_error("cannot open ZIP archive");

        unz_global_info64 global{};
        if (unzGetGlobalInfo64(archive.value, &global) != UNZ_OK)
            throw std::runtime_error("cannot read ZIP directory");
        if (global.number_entry == 0 || global.number_entry > limits_.max_entries)
            throw std::runtime_error("invalid ZIP entry count");

        const auto staging = create_staging_directory(state_dir_ / "staging");
        struct StagingCleanup {
            std::filesystem::path path;
            bool active = true;
            ~StagingCleanup() { if (active) remove_tree(path); }
        } cleanup{ staging };

        std::set<std::string> names;
        std::uint64_t total = 0;
        if (unzGoToFirstFile(archive.value) != UNZ_OK)
            throw std::runtime_error("empty ZIP archive");
        for (std::uint64_t index = 0; index < global.number_entry; ++index) {
            unz_file_info64 info{};
            std::array<char, 4097> name_buffer{};
            if (unzGetCurrentFileInfo64(archive.value, &info, name_buffer.data(),
                name_buffer.size(), nullptr, 0, nullptr, 0) !=
                UNZ_OK)
                throw std::runtime_error("cannot read ZIP entry");
            if (info.size_filename == 0 ||
                info.size_filename >= name_buffer.size())
                throw std::runtime_error("ZIP entry name is too long");
            std::string name(name_buffer.data(), info.size_filename);
            auto destination_name =
                std::filesystem::path(name).lexically_normal().generic_string();
            while (destination_name.size() > 1 &&
                destination_name.ends_with('/'))
                destination_name.pop_back();
            const auto unix_mode = static_cast<unsigned>(info.external_fa >> 16);
            if (unsafe_entry(name) || (!destination_name.empty() &&
                !names.insert(destination_name).second) ||
                ((unix_mode & S_IFMT) == S_IFLNK))
                throw std::runtime_error("unsafe or duplicate ZIP entry: " + name);
            if ((info.flag & 1u) != 0)
                throw std::runtime_error("encrypted ZIP entries are unsupported");
            if (info.compression_method != 0 && info.compression_method != Z_DEFLATED)
                throw std::runtime_error("unsupported ZIP compression method");
            if (info.uncompressed_size > limits_.max_file_bytes ||
                total > limits_.max_uncompressed_bytes - info.uncompressed_size)
                throw std::runtime_error("ZIP extraction limit exceeded");
            total += info.uncompressed_size;

            auto destination = staging / std::filesystem::path(name);
            const bool directory = !name.empty() && name.back() == '/';
            if (directory) {
                std::filesystem::create_directories(destination);
            }
            else {
                std::filesystem::create_directories(destination.parent_path());
                if (unzOpenCurrentFile(archive.value) != UNZ_OK)
                    throw std::runtime_error("cannot open ZIP entry");
                std::ofstream output(destination, std::ios::binary | std::ios::trunc);
                if (!output) {
                    unzCloseCurrentFile(archive.value);
                    throw std::runtime_error("cannot create extracted file");
                }
                std::array<char, 32 * 1024> buffer{};
                std::uint64_t written = 0;
                for (;;) {
                    const int count = unzReadCurrentFile(archive.value, buffer.data(),
                        buffer.size());
                    if (count < 0) {
                        unzCloseCurrentFile(archive.value);
                        throw std::runtime_error("cannot extract ZIP entry");
                    }
                    if (count == 0) break;
                    written += static_cast<unsigned>(count);
                    if (written > info.uncompressed_size ||
                        written > limits_.max_file_bytes) {
                        unzCloseCurrentFile(archive.value);
                        throw std::runtime_error("ZIP entry expanded unexpectedly");
                    }
                    output.write(buffer.data(), count);
                }
                if (!output || written != info.uncompressed_size ||
                    unzCloseCurrentFile(archive.value) != UNZ_OK)
                    throw std::runtime_error("corrupt ZIP entry");
            }
            if (index + 1 < global.number_entry &&
                unzGoToNextFile(archive.value) != UNZ_OK)
                throw std::runtime_error("truncated ZIP directory");
        }

        auto manifest = load_manifest(staging / "meta.json");
        const auto entry = staging / manifest.entry;
        if (!std::filesystem::is_regular_file(entry))
            throw std::runtime_error("package entry does not exist");

        const auto package_root = state_dir_ / "packages" / manifest.id;
        const auto data_dir = state_dir_ / "data" / manifest.id;
        std::filesystem::create_directories(package_root);
        std::filesystem::create_directories(data_dir);
        auto final_dir = package_root /
            (manifest.version + "-" + unique_suffix());
        while (std::filesystem::exists(final_dir))
            final_dir = package_root /
                (manifest.version + "-" + unique_suffix());
        std::filesystem::rename(staging, final_dir);
        cleanup.active = false;

        return StoredPackage{ std::move(manifest), true, final_dir, data_dir };
    }

    void ZipInstaller::discard_release(const StoredPackage& package) noexcept {
        if (is_owned_release(state_dir_, package))
            remove_tree(package.version_dir);
    }

    void ZipInstaller::cleanup_orphans(
        const std::vector<StoredPackage>& packages) noexcept {
        std::error_code error;
        const auto staging = state_dir_ / "staging";
        if (std::filesystem::is_directory(staging, error)) {
            for (std::filesystem::directory_iterator iterator(staging, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                if (iterator->path().filename().string().starts_with("install-"))
                    remove_tree(iterator->path());
            }
        }

        std::unordered_set<std::string> active_ids;
        for (const auto& package : packages) {
            active_ids.insert(package.manifest.id);
        }

        const auto trash_root = state_dir_ / "trash";
        error.clear();
        if (std::filesystem::is_directory(trash_root, error)) {
            for (std::filesystem::directory_iterator transactions(
                     trash_root, error), end;
                 !error && transactions != end; transactions.increment(error)) {
                const auto status = transactions->symlink_status(error);
                if (error || !std::filesystem::is_directory(status) ||
                    std::filesystem::is_symlink(status))
                    continue;
                const auto id = transactions->path().filename().string();
                if (!is_reverse_domain_id(id)) continue;
                if (active_ids.contains(id)) {
                    const auto package_root =
                        state_dir_ / "packages" / id;
                    const auto data_root = state_dir_ / "data" / id;
                    const bool package_staged = std::filesystem::exists(
                        transactions->path() / "package");
                    const bool data_staged = std::filesystem::exists(
                        transactions->path() / "data");
                    const bool package_restored = restore_if_moved(
                        transactions->path() / "package",
                        package_root,
                        package_staged);
                    const bool data_restored = restore_if_moved(
                        transactions->path() / "data", data_root, data_staged);
                    if (package_restored && data_restored)
                        remove_tree(transactions->path());
                }
                else {
                    remove_tree(transactions->path());
                }
            }
        }

        std::unordered_set<std::string> active;
        for (const auto& package : packages) {
            error.clear();
            const auto path = std::filesystem::weakly_canonical(
                package.version_dir, error);
            if (!error) active.insert(path.string());
        }
        const auto packages_root = state_dir_ / "packages";
        error.clear();
        if (!std::filesystem::is_directory(packages_root, error)) return;
        for (std::filesystem::directory_iterator roots(packages_root, error), end;
             !error && roots != end; roots.increment(error)) {
            const auto root_status = roots->symlink_status(error);
            if (error || !std::filesystem::is_directory(root_status) ||
                std::filesystem::is_symlink(root_status) ||
                !is_reverse_domain_id(roots->path().filename().string()))
                continue;
            std::error_code release_error;
            for (std::filesystem::directory_iterator releases(
                     roots->path(), release_error), release_end;
                 !release_error && releases != release_end;
                 releases.increment(release_error)) {
                const auto release_status =
                    releases->symlink_status(release_error);
                if (release_error ||
                    !std::filesystem::is_directory(release_status) ||
                    std::filesystem::is_symlink(release_status))
                    continue;
                const auto canonical = std::filesystem::weakly_canonical(
                    releases->path(), release_error);
                if (!release_error && !active.contains(canonical.string()))
                    remove_tree(releases->path());
            }
        }
    }

    void ZipInstaller::remove(std::string_view package_id, bool purge_data,
        const std::function<void()>& publish) {
        if (!is_reverse_domain_id(package_id))
            throw std::runtime_error("invalid package id");
        const auto id = std::string(package_id);
        const auto trash_root = state_dir_ / "trash";
        std::filesystem::create_directories(trash_root);
        const auto transaction = trash_root / id;
        std::error_code create_error;
        if (!std::filesystem::create_directory(transaction, create_error)) {
            if (!create_error) create_error = std::make_error_code(
                std::errc::file_exists);
            throw std::filesystem::filesystem_error(
                "create removal transaction", transaction, create_error);
        }
        bool package_moved = false;
        bool data_moved = false;
        try {
            rename_if_present(state_dir_ / "packages" / id,
                transaction / "package", package_moved);
            if (purge_data)
                rename_if_present(state_dir_ / "data" / id,
                    transaction / "data", data_moved);
            publish();
        }
        catch (...) {
            const bool data_restored = restore_if_moved(
                transaction / "data", state_dir_ / "data" / id,
                data_moved);
            const bool package_restored = restore_if_moved(
                transaction / "package",
                state_dir_ / "packages" / id, package_moved);
            if (data_restored && package_restored)
                remove_tree(transaction);
            throw;
        }
        remove_tree(transaction);
    }

} // namespace circud
