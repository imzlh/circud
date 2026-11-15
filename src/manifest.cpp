#include <circud/manifest.hpp>

#include "json.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>

namespace circud {
    namespace {

        constexpr std::uintmax_t kMaxManifestBytes = 64 * 1024;
        constexpr std::size_t kMaxEndpointPrefixBytes = 256;

        void reject_unknown(const boost::json::object& object,
            std::initializer_list<std::string_view> allowed,
            std::string_view scope) {
            for (const auto& item : object) {
                bool known = false;
                for (const auto field : allowed) {
                    if (item.key() == field) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    const auto prefix = scope.empty() ? std::string{}
                        : std::string(scope) + ".";
                    throw std::runtime_error("unknown manifest field: " +
                        prefix + std::string(item.key()));
                }
            }
        }

        const boost::json::object& require_object(const boost::json::value& value,
            std::string_view field) {
            if (!value.is_object()) {
                throw std::runtime_error(std::string(field) + " must be an object");
            }
            return value.as_object();
        }

        std::string require_string(const boost::json::object& object,
            std::string_view field) {
            const auto* value = object.if_contains(field);
            if (!value || !value->is_string()) {
                throw std::runtime_error(std::string(field) + " must be a string");
            }
            return std::string(value->as_string());
        }

        bool optional_bool(const boost::json::object& object, std::string_view field,
            bool fallback) {
            const auto* value = object.if_contains(field);
            if (!value) return fallback;
            if (!value->is_bool()) {
                throw std::runtime_error(std::string(field) + " must be boolean");
            }
            return value->as_bool();
        }

        std::vector<std::string> string_array(const boost::json::object& object,
            std::string_view field) {
            std::vector<std::string> result;
            const auto* value = object.if_contains(field);
            if (!value) return result;
            if (!value->is_array()) {
                throw std::runtime_error(std::string(field) + " must be an array");
            }
            for (const auto& item : value->as_array()) {
                if (!item.is_string()) {
                    throw std::runtime_error(std::string(field) +
                        " entries must be strings");
                }
                result.emplace_back(item.as_string());
            }
            return result;
        }

        bool is_endpoint_prefix(std::string_view value) noexcept {
            if (value.empty() || value.size() > kMaxEndpointPrefixBytes ||
                value.front() != '/' || value == "/")
                return false;
            if (value.back() == '/') return false;

            std::size_t segment_start = 1;
            for (std::size_t index = 1; index <= value.size(); ++index) {
                if (index != value.size() && value[index] != '/') {
                    const auto character = value[index];
                    const auto alpha_numeric =
                        (character >= 'a' && character <= 'z') ||
                        (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9');
                    if (!alpha_numeric && character != '-' && character != '.' &&
                        character != '_' && character != '~')
                        return false;
                    continue;
                }
                if (index == segment_start) return false;
                const auto segment = value.substr(segment_start,
                    index - segment_start);
                if (segment == "." || segment == "..") return false;
                segment_start = index + 1;
            }
            return true;
        }

        bool is_path_or_descendant(std::string_view path,
            std::string_view base) noexcept {
            return path == base ||
                (path.size() > base.size() && path.starts_with(base) &&
                    path[base.size()] == '/');
        }

        void validate_endpoint_prefix(std::string_view prefix) {
            if (!is_endpoint_prefix(prefix))
                throw std::runtime_error("prefix is invalid");
            constexpr std::string_view reserved_paths[] = {
                "/health", "/rpc", "/api",
                "/index.html", "/app.js", "/app.css",
                "/circud-api.d.ts", "/assets", "/favicon.ico",
            };
            for (const auto path : reserved_paths) {
                if (is_path_or_descendant(prefix, path))
                    throw std::runtime_error(
                        "prefix overlaps a reserved path");
            }
        }

    } // namespace

    bool is_reverse_domain_id(std::string_view value) noexcept {
        static const std::regex pattern(
            R"(^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$)",
            std::regex::ECMAScript);
        return value.size() <= 253 &&
            std::regex_match(value.begin(), value.end(), pattern);
    }

    bool is_semver(std::string_view value) noexcept {
        static const std::regex pattern(
            R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)");
        return value.size() <= 128 &&
            std::regex_match(value.begin(), value.end(), pattern);
    }

    PackageManifest parse_manifest(std::string_view json_text) {
        const auto root_value = parse_json(json_text);
        const auto& root = require_object(root_value, "manifest");
        reject_unknown(root,
            { "schema", "id", "name", "version", "entry",
              "acceptPackageCalls", "shutdownTimeoutMs", "prefix",
              "permissions" },
            "");
        PackageManifest manifest;

        const auto* schema = root.if_contains("schema");
        if (!schema || !schema->is_int64() || schema->as_int64() != 1) {
            throw std::runtime_error("unsupported manifest schema");
        }
        manifest.id = require_string(root, "id");
        manifest.name = require_string(root, "name");
        manifest.version = require_string(root, "version");
        if (!is_reverse_domain_id(manifest.id))
            throw std::runtime_error("invalid package id");
        if (manifest.name.empty() || manifest.name.size() > 128)
            throw std::runtime_error("invalid package name");
        if (!is_semver(manifest.version))
            throw std::runtime_error("invalid package version");

        if (const auto* prefix = root.if_contains("prefix")) {
            if (!prefix->is_string())
                throw std::runtime_error("prefix must be a string");
            manifest.endpoint_prefix = std::string(prefix->as_string());
            validate_endpoint_prefix(manifest.endpoint_prefix);
        }

        if (const auto* entry = root.if_contains("entry")) {
            if (!entry->is_string())
                throw std::runtime_error("entry must be a string");
            manifest.entry = std::string(entry->as_string());
        }
        if (manifest.entry.empty() || manifest.entry.is_absolute() ||
            manifest.entry.extension() != ".js")
            throw std::runtime_error("entry must be a relative .js path");
        for (const auto& part : manifest.entry) {
            if (part == "..") throw std::runtime_error("entry escapes package");
        }

        manifest.accept_package_calls =
            optional_bool(root, "acceptPackageCalls", true);
        if (const auto* timeout = root.if_contains("shutdownTimeoutMs")) {
            if (!timeout->is_int64() || timeout->as_int64() < 100 ||
                timeout->as_int64() > 30000)
                throw std::runtime_error("shutdownTimeoutMs must be 100..30000");
            manifest.shutdown_timeout =
                std::chrono::milliseconds(timeout->as_int64());
        }
        if (const auto* permissions = root.if_contains("permissions")) {
            const auto& p = require_object(*permissions, "permissions");
            reject_unknown(p,
                { "read", "write", "env", "net", "run", "ffi", "sys" },
                "permissions");
            manifest.permissions.read = string_array(p, "read");
            manifest.permissions.write = string_array(p, "write");
            manifest.permissions.env = string_array(p, "env");
            manifest.permissions.net = optional_bool(p, "net", false);
            manifest.permissions.run = optional_bool(p, "run", false);
            manifest.permissions.ffi = optional_bool(p, "ffi", false);
            manifest.permissions.sys = optional_bool(p, "sys", false);
        }
        return manifest;
    }

    std::string effective_endpoint_prefix(const PackageManifest& manifest) {
        if (!manifest.endpoint_prefix.empty()) return manifest.endpoint_prefix;
        return "/" + manifest.id;
    }

    PackageManifest load_manifest(const std::filesystem::path& path) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) throw std::runtime_error("cannot inspect " + path.string());
        if (size > kMaxManifestBytes)
            throw std::runtime_error("manifest exceeds 64 KiB");
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open " + path.string());
        std::string text((std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        return parse_manifest(text);
    }

    boost::json::value manifest_to_json(const PackageManifest& manifest) {
        boost::json::object permissions;
        auto make_array = [](const std::vector<std::string>& values) {
            boost::json::array result;
            for (const auto& value : values) result.emplace_back(value);
            return result;
            };
        permissions["read"] = make_array(manifest.permissions.read);
        permissions["write"] = make_array(manifest.permissions.write);
        permissions["env"] = make_array(manifest.permissions.env);
        permissions["net"] = manifest.permissions.net;
        permissions["run"] = manifest.permissions.run;
        permissions["ffi"] = manifest.permissions.ffi;
        permissions["sys"] = manifest.permissions.sys;

        boost::json::object result{
            { "schema", manifest.schema },
            { "id", manifest.id },
            { "name", manifest.name },
            { "version", manifest.version },
            { "entry", manifest.entry.generic_string() },
            { "acceptPackageCalls", manifest.accept_package_calls },
            { "shutdownTimeoutMs", manifest.shutdown_timeout.count() },
            { "permissions", std::move(permissions) },
        };
        if (!manifest.endpoint_prefix.empty())
            result["prefix"] = manifest.endpoint_prefix;
        return result;
    }

} // namespace circud
