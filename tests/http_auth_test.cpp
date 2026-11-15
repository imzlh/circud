#include "test.hpp"

#include <circud/http_auth.hpp>

#include <chrono>
#include <string>

namespace {

    std::string session_token(char value = 'A') {
        return std::string(43, value);
    }

} // namespace

CIRCUD_TEST(http_auth_validates_strict_origin_targets) {
    const auto root = circud::validate_origin_target("/");
    circud::test::require(static_cast<bool>(root));
    circud::test::require(root.path == "/");
    circud::test::require(root.query.empty());
    circud::test::require(!root.has_query);

    const auto api = circud::validate_origin_target(
        "/api/auth/session?fresh=1&next=%3F");
    circud::test::require(static_cast<bool>(api));
    circud::test::require(api.path == "/api/auth/session");
    circud::test::require(api.query == "fresh=1&next=%3F");
    circud::test::require(api.has_query);

    const auto escaped = circud::validate_origin_target("/a%20b/%7Euser");
    circud::test::require(static_cast<bool>(escaped));
    circud::test::require(escaped.path == "/a%20b/~user");

    const auto core_alias =
        circud::validate_origin_target("/ap%69/unknown");
    circud::test::require(static_cast<bool>(core_alias));
    circud::test::require(core_alias.path == "/api/unknown");

    const auto package_alias = circud::validate_origin_target(
        "/io.example.endpo%69nt/method?x=%69&next=%3f");
    circud::test::require(static_cast<bool>(package_alias));
    circud::test::require(
        package_alias.path == "/io.example.endpoint/method");
    circud::test::require(package_alias.query == "x=%69&next=%3f");

    const auto reserved_escape =
        circud::validate_origin_target("/question%3fmark");
    circud::test::require(static_cast<bool>(reserved_escape));
    circud::test::require(reserved_escape.path == "/question%3Fmark");

    const auto empty_query = circud::validate_origin_target("/path?");
    circud::test::require(static_cast<bool>(empty_query));
    circud::test::require(empty_query.has_query);
    circud::test::require(empty_query.query.empty());
}

CIRCUD_TEST(http_auth_rejects_ambiguous_or_unsafe_origin_targets) {
    using Status = circud::OriginTargetStatus;
    const auto require_status = [](std::string_view target, Status status) {
        const auto result = circud::validate_origin_target(target);
        circud::test::require(result.status == status,
            std::string("unexpected target status for ") +
                std::string(target));
    };

    require_status("", Status::invalid_form);
    require_status("*", Status::invalid_form);
    require_status("https://localhost/api", Status::invalid_form);
    require_status("/api#fragment", Status::fragment);
    require_status("/api\\child", Status::backslash);
    require_status("/api child", Status::invalid_character);
    require_status(std::string("/api\x01", 5), Status::invalid_character);
    require_status("/api%", Status::malformed_percent_encoding);
    require_status("/api%2", Status::malformed_percent_encoding);
    require_status("/api%XZ", Status::malformed_percent_encoding);
    require_status("/api%2Fauth", Status::encoded_separator);
    require_status("/api%2fauth", Status::encoded_separator);
    require_status("/api%5Cauth", Status::encoded_separator);
    require_status("/api%00auth", Status::encoded_separator);
    require_status("/api%0Aauth", Status::invalid_character);
    require_status("/api%7Fauth", Status::invalid_character);
    require_status("/./health", Status::invalid_character);
    require_status("/../health", Status::invalid_character);
    require_status("/%2e/health", Status::invalid_character);
    require_status("/%2E%2e/health", Status::invalid_character);

    std::string oversized(circud::kMaxHttpOriginTargetBytes + 1, 'a');
    oversized.front() = '/';
    require_status(oversized, Status::too_large);
}

CIRCUD_TEST(http_auth_parses_one_strict_session_cookie) {
    const auto token = session_token();
    const auto absent = circud::parse_session_cookie("theme=dark; count=2");
    circud::test::require(absent.status ==
        circud::SessionCookieStatus::absent);

    const auto found_cookie =
        "theme=dark; circud_session=" + token + "; count=2";
    const auto found = circud::parse_session_cookie(found_cookie);
    circud::test::require(static_cast<bool>(found));
    circud::test::require(found.token == token);

    const auto quoted_cookie = "circud_session=\"" + token + "\"";
    const auto quoted = circud::parse_session_cookie(quoted_cookie);
    circud::test::require(static_cast<bool>(quoted));
    circud::test::require(quoted.token == token);

    const auto empty = circud::parse_session_cookie({});
    circud::test::require(empty.status ==
        circud::SessionCookieStatus::absent);
}

CIRCUD_TEST(http_auth_rejects_duplicate_or_malformed_cookies) {
    const auto token = session_token();
    const auto require_invalid = [](const std::string& value) {
        circud::test::require(circud::parse_session_cookie(value).status ==
            circud::SessionCookieStatus::invalid, value);
    };

    require_invalid("circud_session=" + token +
        "; circud_session=" + token);
    require_invalid("circud_session=short");
    require_invalid("circud_session=" + std::string(42, 'A') + "!");
    require_invalid("no-equals");
    require_invalid("name=value;");
    require_invalid("=value");
    require_invalid("name =value");
    require_invalid("name= value");
    require_invalid("name=contains space");
    require_invalid("name=\"unterminated");

    std::string oversized(circud::kMaxHttpCookieBytes + 1, 'x');
    require_invalid(oversized);
}

CIRCUD_TEST(http_auth_strips_only_the_core_session_cookie) {
    const auto token = session_token();
    const auto stripped = circud::strip_core_session_cookie(
        "theme=dark; circud_session=" + token + "; quoted=\"yes\"");
    circud::test::require(stripped.status ==
        circud::SessionCookieStatus::found);
    circud::test::require(stripped.value.has_value());
    circud::test::require(*stripped.value ==
        "theme=dark; quoted=\"yes\"");

    const auto only = circud::strip_core_session_cookie(
        "circud_session=" + token);
    circud::test::require(only.status ==
        circud::SessionCookieStatus::found);
    circud::test::require(!only.value);

    const auto unrelated = circud::strip_core_session_cookie(
        "theme=dark; count=2");
    circud::test::require(unrelated.status ==
        circud::SessionCookieStatus::absent);
    circud::test::require(unrelated.value == "theme=dark; count=2");

    const auto invalid = circud::strip_core_session_cookie(
        "circud_session=short; theme=dark");
    circud::test::require(invalid.status ==
        circud::SessionCookieStatus::invalid);
    circud::test::require(!invalid.value);
}

CIRCUD_TEST(http_auth_builds_bounded_session_cookie_headers) {
    using namespace std::chrono_literals;
    const auto token = session_token('_');
    const auto plain = circud::build_session_set_cookie(token, false, 30min);
    circud::test::require(plain.has_value());
    circud::test::require(*plain == "circud_session=" + token +
        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800");

    const auto secure = circud::build_session_set_cookie(token, true, 1s);
    circud::test::require(secure.has_value());
    circud::test::require(secure->find("; Secure; Max-Age=1") !=
        std::string::npos);
    circud::test::require(!circud::build_session_set_cookie(
        "short", true, 1s));
    circud::test::require(!circud::build_session_set_cookie(
        token, true, 0s));

    circud::test::require(circud::build_session_clear_cookie(false) ==
        "circud_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    circud::test::require(circud::build_session_clear_cookie(true) ==
        "circud_session=; Path=/; HttpOnly; SameSite=Strict; Secure; "
        "Max-Age=0");
}
