#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace circud {

    inline constexpr std::string_view kCircudSessionCookie =
        "circud_session";
    inline constexpr std::size_t kMaxHttpOriginTargetBytes = 8 * 1024;
    inline constexpr std::size_t kMaxHttpCookieBytes = 8 * 1024;
    inline constexpr std::size_t kMaxHttpCookiePairs = 128;

    enum class OriginTargetStatus {
        valid,
        too_large,
        invalid_form,
        fragment,
        backslash,
        invalid_character,
        malformed_percent_encoding,
        encoded_separator,
    };

    // path is an owned canonical routing path. Percent-encoded RFC unreserved
    // characters are decoded and other accepted path escapes use uppercase hex.
    // query borrows from the supplied target, excludes '?', and is never
    // decoded or otherwise normalized. has_query distinguishes /path from
    // /path?.
    struct OriginTarget {
        OriginTargetStatus status = OriginTargetStatus::invalid_form;
        std::string path;
        std::string_view query;
        bool has_query = false;

        constexpr explicit operator bool() const noexcept {
            return status == OriginTargetStatus::valid;
        }
    };

    // Accepts only a bounded RFC origin-form target and produces the one path
    // representation used by core, package and static-file routing. It rejects
    // fragments, raw controls/backslashes, malformed percent escapes, dot
    // segments, and percent-encoded slash, backslash, NUL or controls.
    // Rejecting encoded separators and canonicalizing unreserved characters
    // before routing prevents /ap%69 and /api%2F... boundary bypasses.
    OriginTarget validate_origin_target(std::string_view target) noexcept;

    enum class SessionCookieStatus {
        absent,
        found,
        invalid,
    };

    struct SessionCookie {
        SessionCookieStatus status = SessionCookieStatus::absent;
        // Borrows from the Cookie field value.
        std::string_view token;

        constexpr explicit operator bool() const noexcept {
            return status == SessionCookieStatus::found;
        }
    };

    // Parse one logical Cookie value. A caller with multiple Cookie fields
    // must join them with "; " while enforcing kMaxHttpCookieBytes. The core
    // session cookie must occur exactly once and contain the fixed-size,
    // unpadded base64url bearer token produced by AuthManager. Any malformed
    // cookie pair, duplicate core cookie, or invalid core token is invalid.
    SessionCookie parse_session_cookie(std::string_view cookie) noexcept;

    struct StrippedSessionCookie {
        SessionCookieStatus status = SessionCookieStatus::absent;
        // nullopt means there is no Cookie field left to forward.
        std::optional<std::string> value;
    };

    // Validate the complete Cookie value and remove every core session cookie.
    // Invalid input produces no forwardable value. Other cookies are retained
    // in a canonical "name=value; name=value" form.
    StrippedSessionCookie strip_core_session_cookie(
        std::string_view cookie);

    // Return nullopt for an invalid token or an out-of-range lifetime. The
    // result is bounded and safe to use directly as a Set-Cookie field value.
    std::optional<std::string> build_session_set_cookie(
        std::string_view token, bool secure,
        std::chrono::seconds max_age);

    // Expire the core cookie at Path=/ using Max-Age=0.
    std::string build_session_clear_cookie(bool secure);

} // namespace circud
