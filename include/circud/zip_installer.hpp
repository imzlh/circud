#pragma once

#include <circud/database.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace circud {

    struct ZipLimits {
        std::uint64_t max_archive_bytes = 64 * 1024 * 1024;
        std::uint64_t max_uncompressed_bytes = 256 * 1024 * 1024;
        std::uint64_t max_file_bytes = 32 * 1024 * 1024;
        std::size_t max_entries = 4096;
    };

    class ZipInstaller {
    public:
        explicit ZipInstaller(std::filesystem::path state_dir,
            ZipLimits limits = {});

        StoredPackage install(const std::filesystem::path& archive);
        void cleanup_orphans(const std::vector<StoredPackage>& packages) noexcept;
        void discard_release(const StoredPackage& package) noexcept;
        void remove(std::string_view package_id, bool purge_data,
            const std::function<void()>& publish);

    private:
        std::filesystem::path state_dir_;
        ZipLimits limits_;
    };

} // namespace circud
