#include "test.hpp"

#include <circud/http_route.hpp>

#include <array>
#include <string_view>

namespace {

    using circud::CoreHttpAuth;
    using circud::CoreHttpBodyPolicy;
    using circud::CoreHttpHandler;
    using circud::CoreHttpRouteStatus;

    struct ExpectedRoute {
        std::string_view method;
        std::string_view target;
        CoreHttpHandler handler;
        CoreHttpAuth auth;
        bool csrf;
        CoreHttpBodyPolicy body;
        std::string_view allow;
    };

} // namespace

CIRCUD_TEST(core_http_route_table_matches_declared_contract) {
    constexpr std::array expected{
        ExpectedRoute{ "GET", "/health", CoreHttpHandler::health,
            CoreHttpAuth::public_, false, CoreHttpBodyPolicy::none,
            "GET, HEAD" },
        ExpectedRoute{ "HEAD", "/health", CoreHttpHandler::health,
            CoreHttpAuth::public_, false, CoreHttpBodyPolicy::none,
            "GET, HEAD" },
        ExpectedRoute{ "POST", "/rpc", CoreHttpHandler::rpc,
            CoreHttpAuth::protected_, true, CoreHttpBodyPolicy::json, "POST" },
        ExpectedRoute{ "GET", "/api/auth/session",
            CoreHttpHandler::auth_session, CoreHttpAuth::public_, false,
            CoreHttpBodyPolicy::none, "GET" },
        ExpectedRoute{ "POST", "/api/auth/login",
            CoreHttpHandler::auth_login, CoreHttpAuth::public_, false,
            CoreHttpBodyPolicy::json, "POST" },
        ExpectedRoute{ "POST", "/api/auth/logout",
            CoreHttpHandler::auth_logout, CoreHttpAuth::protected_, true,
            CoreHttpBodyPolicy::none, "POST" },
        ExpectedRoute{ "POST", "/api/packages/install",
            CoreHttpHandler::packages_install, CoreHttpAuth::protected_, true,
            CoreHttpBodyPolicy::zip_upload, "POST" },
    };

    for (const auto& item : expected) {
        const auto route = circud::route_core_http(item.method, item.target);
        circud::test::require(route.status == CoreHttpRouteStatus::matched);
        circud::test::require(route.handler == item.handler);
        circud::test::require(route.auth == item.auth);
        circud::test::require(route.csrf == item.csrf);
        circud::test::require(route.body == item.body);
        circud::test::require(route.allow == item.allow);
        circud::test::require(route.reserved());
        circud::test::require(static_cast<bool>(route));
    }
}

CIRCUD_TEST(core_http_route_table_reports_405_with_allow_and_metadata) {
    const auto health = circud::route_core_http("POST", "/health");
    circud::test::require(health.status ==
        CoreHttpRouteStatus::method_not_allowed);
    circud::test::require(health.handler == CoreHttpHandler::health);
    circud::test::require(health.allow == "GET, HEAD");

    const auto install = circud::route_core_http(
        "PROPFIND", "/api/packages/install");
    circud::test::require(install.status ==
        CoreHttpRouteStatus::method_not_allowed);
    circud::test::require(install.handler ==
        CoreHttpHandler::packages_install);
    circud::test::require(install.auth == CoreHttpAuth::protected_);
    circud::test::require(install.csrf);
    circud::test::require(install.body == CoreHttpBodyPolicy::zip_upload);
    circud::test::require(install.allow == "POST");
}

CIRCUD_TEST(core_http_route_table_reserves_the_api_tree) {
    for (const auto target : { "/api", "/api/", "/api/unknown",
            "/api/auth/session/child", "/api/packages" }) {
        const auto route = circud::route_core_http("GET", target);
        circud::test::require(route.status == CoreHttpRouteStatus::not_found,
            std::string("expected reserved 404 for ") + target);
        circud::test::require(route.reserved());
        circud::test::require(route.handler == CoreHttpHandler::none);
        circud::test::require(route.allow.empty());
    }
}

CIRCUD_TEST(core_http_route_table_passes_only_unowned_targets) {
    for (const auto target : { "/", "/app.js", "/apiary",
            "/package/endpoint" }) {
        const auto route = circud::route_core_http("GET", target);
        circud::test::require(route.status == CoreHttpRouteStatus::pass,
            std::string("expected pass for ") + target);
        circud::test::require(!route.reserved());
    }

    // HTTP method tokens are case-sensitive.
    const auto lower = circud::route_core_http("get", "/health");
    circud::test::require(lower.status ==
        CoreHttpRouteStatus::method_not_allowed);
    circud::test::require(lower.allow == "GET, HEAD");
}
