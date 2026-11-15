#pragma once

#include <boost/json/value.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace circud {

    struct PermissionSet {
        std::vector<std::string> read;
        std::vector<std::string> write;
        std::vector<std::string> env;
        bool net = false;
        bool run = false;
        bool ffi = false;
        bool sys = false;
    };

    struct PackageManifest {
        unsigned schema = 1;
        std::string id;
        std::string name;
        std::string version;
        std::filesystem::path entry = "management.js";
        bool accept_package_calls = true;
        std::chrono::milliseconds shutdown_timeout{ 5000 };
        // Empty selects the package-owned default: /<package-id>.
        // Stored prefixes are canonical bases without a trailing slash.
        std::string endpoint_prefix;
        PermissionSet permissions;
    };

    std::string effective_endpoint_prefix(const PackageManifest& manifest);
    PackageManifest parse_manifest(std::string_view json_text);
    PackageManifest load_manifest(const std::filesystem::path& path);
    boost::json::value manifest_to_json(const PackageManifest& manifest);
    bool is_reverse_domain_id(std::string_view value) noexcept;
    bool is_semver(std::string_view value) noexcept;

} // namespace circud
