#include <circud/http_route.hpp>

#include <array>

namespace circud {
    namespace {

        struct RouteDefinition {
            std::string_view path;
            std::string_view method;
            std::string_view alternate_method;
            std::string_view allow;
            CoreHttpHandler handler;
            CoreHttpAuth auth;
            bool csrf;
            CoreHttpBodyPolicy body;
        };

        constexpr std::array routes{
            RouteDefinition{ "/health", "GET", "HEAD", "GET, HEAD",
                CoreHttpHandler::health, CoreHttpAuth::public_, false,
                CoreHttpBodyPolicy::none },
            RouteDefinition{ "/rpc", "POST", {}, "POST",
                CoreHttpHandler::rpc, CoreHttpAuth::protected_, true,
                CoreHttpBodyPolicy::json },
            RouteDefinition{ "/api/auth/session", "GET", {}, "GET",
                CoreHttpHandler::auth_session, CoreHttpAuth::public_, false,
                CoreHttpBodyPolicy::none },
            RouteDefinition{ "/api/auth/login", "POST", {}, "POST",
                CoreHttpHandler::auth_login, CoreHttpAuth::public_, false,
                CoreHttpBodyPolicy::json },
            RouteDefinition{ "/api/auth/logout", "POST", {}, "POST",
                CoreHttpHandler::auth_logout, CoreHttpAuth::protected_, true,
                CoreHttpBodyPolicy::none },
            RouteDefinition{ "/api/packages/install", "POST", {}, "POST",
                CoreHttpHandler::packages_install,
                CoreHttpAuth::protected_, true,
                CoreHttpBodyPolicy::zip_upload },
        };

        bool method_is(const RouteDefinition& route,
            std::string_view candidate) noexcept {
            return candidate == route.method ||
                (!route.alternate_method.empty() &&
                    candidate == route.alternate_method);
        }

        bool api_path(std::string_view path) noexcept {
            return path == "/api" || path.starts_with("/api/");
        }

        CoreHttpRoute result_for(const RouteDefinition& route,
            CoreHttpRouteStatus status) noexcept {
            return CoreHttpRoute{ status, route.handler, route.auth,
                route.csrf, route.body, route.allow };
        }

    } // namespace

    CoreHttpRoute route_core_http(std::string_view method,
        std::string_view path) noexcept {
        if (path.empty() || path.front() != '/' ||
            path.find_first_of("?#") != std::string_view::npos)
            return {};
        for (const auto& route : routes) {
            if (route.path != path) continue;
            return result_for(route, method_is(route, method)
                ? CoreHttpRouteStatus::matched
                : CoreHttpRouteStatus::method_not_allowed);
        }

        if (api_path(path)) {
            CoreHttpRoute result;
            result.status = CoreHttpRouteStatus::not_found;
            return result;
        }
        return {};
    }

} // namespace circud
