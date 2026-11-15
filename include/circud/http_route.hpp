#pragma once

#include <string_view>

namespace circud {

    enum class CoreHttpRouteStatus {
        // The target is not owned by the core router. Package and Web routing
        // may continue.
        pass,
        matched,
        not_found,
        method_not_allowed,
    };

    enum class CoreHttpHandler {
        none,
        health,
        rpc,
        auth_session,
        auth_login,
        auth_logout,
        packages_install,
    };

    enum class CoreHttpAuth {
        public_,
        protected_,
    };

    enum class CoreHttpBodyPolicy {
        none,
        json,
        zip_upload,
    };

    struct CoreHttpRoute {
        CoreHttpRouteStatus status = CoreHttpRouteStatus::pass;
        CoreHttpHandler handler = CoreHttpHandler::none;
        CoreHttpAuth auth = CoreHttpAuth::public_;
        bool csrf = false;
        CoreHttpBodyPolicy body = CoreHttpBodyPolicy::none;

        // Stable, comma-separated value suitable for an Allow header. It is
        // populated for both matched and method_not_allowed results so callers
        // do not need a second route lookup.
        std::string_view allow;

        constexpr explicit operator bool() const noexcept {
            return status == CoreHttpRouteStatus::matched;
        }

        constexpr bool reserved() const noexcept {
            return status != CoreHttpRouteStatus::pass;
        }
    };

    // Resolve a case-sensitive HTTP method token and the canonical path returned
    // by validate_origin_target(). Validation and query handling belong to the
    // protocol layer; this table compares the already-normalized path only.
    //
    // /api and every /api/... path are reserved. An unknown reserved path is
    // therefore not_found rather than pass.
    CoreHttpRoute route_core_http(std::string_view method,
        std::string_view canonical_path) noexcept;

} // namespace circud
