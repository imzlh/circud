#include <circud/http_auth.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>
#include <vector>

namespace circud {
    namespace {

        constexpr std::size_t kSessionTokenBytes = 43;
        constexpr std::size_t kMaxCookieNameBytes = 256;
        constexpr std::size_t kMaxCookieValueBytes = 4 * 1024;
        constexpr std::int64_t kMaxCookieAgeSeconds =
            10LL * 365 * 24 * 60 * 60;

        int hex_digit(char value) noexcept {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }

        bool uri_character(unsigned char value) noexcept {
            if (std::isalnum(value)) return true;
            constexpr std::string_view allowed =
                "-._~!$&'()*+,;=:@/?";
            return allowed.find(static_cast<char>(value)) !=
                std::string_view::npos;
        }

        bool unreserved_character(unsigned char value) noexcept {
            return std::isalnum(value) || value == '-' || value == '.' ||
                value == '_' || value == '~';
        }

        char upper_hex_digit(unsigned value) noexcept {
            constexpr std::string_view digits = "0123456789ABCDEF";
            return digits[value & 0x0f];
        }

        bool has_dot_segment(std::string_view path) noexcept {
            std::size_t begin = 1;
            for (;;) {
                const auto end = path.find('/', begin);
                const auto segment = path.substr(begin, end - begin);
                if (segment == "." || segment == "..") return true;
                if (end == std::string_view::npos) return false;
                begin = end + 1;
            }
        }

        bool token_character(unsigned char value) noexcept {
            if (std::isalnum(value)) return true;
            constexpr std::string_view punctuation =
                "!#$%&'*+-.^_`|~";
            return punctuation.find(static_cast<char>(value)) !=
                std::string_view::npos;
        }

        bool cookie_octet(unsigned char value) noexcept {
            return value == 0x21 || (value >= 0x23 && value <= 0x2b) ||
                (value >= 0x2d && value <= 0x3a) ||
                (value >= 0x3c && value <= 0x5b) ||
                (value >= 0x5d && value <= 0x7e);
        }

        std::string_view trim_ows(std::string_view value) noexcept {
            while (!value.empty() &&
                (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() &&
                (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            return value;
        }

        bool session_token(std::string_view value) noexcept {
            return value.size() == kSessionTokenBytes &&
                std::ranges::all_of(value, [](unsigned char character) {
                    return std::isalnum(character) || character == '-' ||
                        character == '_';
                });
        }

        struct CookiePair {
            std::string_view name;
            std::string_view value;
            std::string_view serialized_value;
        };

        struct ParsedCookies {
            bool valid = true;
            std::vector<CookiePair> pairs;
        };

        ParsedCookies parse_cookie_pairs(std::string_view cookie) {
            ParsedCookies parsed;
            if (cookie.empty()) return parsed;
            if (cookie.size() > kMaxHttpCookieBytes) {
                parsed.valid = false;
                return parsed;
            }

            std::string_view remaining = cookie;
            while (!remaining.empty()) {
                if (parsed.pairs.size() >= kMaxHttpCookiePairs) {
                    parsed.valid = false;
                    return parsed;
                }
                const auto semicolon = remaining.find(';');
                const auto raw = remaining.substr(0, semicolon);
                const auto item = trim_ows(raw);
                if (item.empty()) {
                    parsed.valid = false;
                    return parsed;
                }
                const auto equals = item.find('=');
                if (equals == std::string_view::npos) {
                    parsed.valid = false;
                    return parsed;
                }
                const auto raw_name = item.substr(0, equals);
                const auto raw_value = item.substr(equals + 1);
                const auto name = trim_ows(raw_name);
                auto serialized_value = trim_ows(raw_value);
                if (name.size() != raw_name.size() ||
                    serialized_value.size() != raw_value.size()) {
                    parsed.valid = false;
                    return parsed;
                }
                if (name.empty() || name.size() > kMaxCookieNameBytes ||
                    serialized_value.size() > kMaxCookieValueBytes ||
                    !std::ranges::all_of(name,
                        [](unsigned char character) {
                            return token_character(character);
                        })) {
                    parsed.valid = false;
                    return parsed;
                }

                auto value = serialized_value;
                if (value.size() >= 2 && value.front() == '"' &&
                    value.back() == '"') {
                    value.remove_prefix(1);
                    value.remove_suffix(1);
                }
                else if ((!value.empty() && value.front() == '"') ||
                    (!value.empty() && value.back() == '"')) {
                    parsed.valid = false;
                    return parsed;
                }
                if (!std::ranges::all_of(value,
                    [](unsigned char character) {
                        return cookie_octet(character);
                    })) {
                    parsed.valid = false;
                    return parsed;
                }
                parsed.pairs.push_back(
                    CookiePair{ name, value, serialized_value });

                if (semicolon == std::string_view::npos) break;
                remaining.remove_prefix(semicolon + 1);
                if (remaining.empty()) {
                    parsed.valid = false;
                    return parsed;
                }
            }
            return parsed;
        }

        SessionCookie session_from(const ParsedCookies& parsed) noexcept {
            if (!parsed.valid) return { SessionCookieStatus::invalid, {} };
            std::string_view token;
            bool found = false;
            for (const auto& pair : parsed.pairs) {
                if (pair.name != kCircudSessionCookie) continue;
                if (found || !session_token(pair.value))
                    return { SessionCookieStatus::invalid, {} };
                found = true;
                token = pair.value;
            }
            return found
                ? SessionCookie{ SessionCookieStatus::found, token }
                : SessionCookie{};
        }

        void append_cookie_attributes(std::string& result, bool secure) {
            result += "; Path=/; HttpOnly; SameSite=Strict";
            if (secure) result += "; Secure";
        }

    } // namespace

    OriginTarget validate_origin_target(std::string_view target) noexcept {
        OriginTarget result;
        if (target.size() > kMaxHttpOriginTargetBytes) {
            result.status = OriginTargetStatus::too_large;
            return result;
        }
        if (target.empty() || target.front() != '/') return result;

        try {
            const auto query = target.find('?');
            const auto path_end = query == std::string_view::npos
                ? target.size() : query;
            result.path.reserve(path_end);
            for (std::size_t index = 0; index < target.size(); ++index) {
                const auto character =
                    static_cast<unsigned char>(target[index]);
                if (character == '#') {
                    result.status = OriginTargetStatus::fragment;
                    return result;
                }
                if (character == '\\') {
                    result.status = OriginTargetStatus::backslash;
                    return result;
                }
                if (character == '%') {
                    if (index + 2 >= target.size()) {
                        result.status =
                            OriginTargetStatus::malformed_percent_encoding;
                        return result;
                    }
                    const auto high = hex_digit(target[index + 1]);
                    const auto low = hex_digit(target[index + 2]);
                    if (high < 0 || low < 0) {
                        result.status =
                            OriginTargetStatus::malformed_percent_encoding;
                        return result;
                    }
                    const auto decoded = static_cast<unsigned char>(
                        (high << 4) | low);
                    if (decoded == 0 || decoded == '/' || decoded == '\\') {
                        result.status = OriginTargetStatus::encoded_separator;
                        return result;
                    }
                    if (decoded < 0x20 || decoded == 0x7f) {
                        result.status = OriginTargetStatus::invalid_character;
                        return result;
                    }
                    if (index < path_end) {
                        if (unreserved_character(decoded)) {
                            result.path.push_back(static_cast<char>(decoded));
                        }
                        else {
                            result.path.push_back('%');
                            result.path.push_back(upper_hex_digit(decoded >> 4));
                            result.path.push_back(upper_hex_digit(decoded));
                        }
                    }
                    index += 2;
                    continue;
                }
                if (character < 0x21 || character > 0x7e ||
                    !uri_character(character)) {
                    result.status = OriginTargetStatus::invalid_character;
                    return result;
                }
                if (index < path_end)
                    result.path.push_back(static_cast<char>(character));
            }

            if (has_dot_segment(result.path)) {
                result.status = OriginTargetStatus::invalid_character;
                return result;
            }
            result.status = OriginTargetStatus::valid;
            result.has_query = query != std::string_view::npos;
            if (result.has_query) result.query = target.substr(query + 1);
            return result;
        }
        catch (...) {
            result.path.clear();
            result.query = {};
            result.has_query = false;
            result.status = OriginTargetStatus::too_large;
            return result;
        }
    }

    SessionCookie parse_session_cookie(std::string_view cookie) noexcept {
        try {
            return session_from(parse_cookie_pairs(cookie));
        }
        catch (...) {
            return { SessionCookieStatus::invalid, {} };
        }
    }

    StrippedSessionCookie strip_core_session_cookie(
        std::string_view cookie) {
        const auto parsed = parse_cookie_pairs(cookie);
        const auto session = session_from(parsed);
        if (session.status == SessionCookieStatus::invalid)
            return { SessionCookieStatus::invalid, std::nullopt };

        std::string forwarded;
        for (const auto& pair : parsed.pairs) {
            if (pair.name == kCircudSessionCookie) continue;
            if (!forwarded.empty()) forwarded += "; ";
            forwarded.append(pair.name);
            forwarded.push_back('=');
            forwarded.append(pair.serialized_value);
        }
        return { session.status, forwarded.empty()
            ? std::nullopt
            : std::optional<std::string>(std::move(forwarded)) };
    }

    std::optional<std::string> build_session_set_cookie(
        std::string_view token, bool secure,
        std::chrono::seconds max_age) {
        const auto age = max_age.count();
        if (!session_token(token) || age <= 0 ||
            age > kMaxCookieAgeSeconds)
            return std::nullopt;
        std::string result;
        result.reserve(token.size() + 80);
        result.append(kCircudSessionCookie);
        result.push_back('=');
        result.append(token);
        append_cookie_attributes(result, secure);
        result += "; Max-Age=";
        result += std::to_string(age);
        return result;
    }

    std::string build_session_clear_cookie(bool secure) {
        std::string result(kCircudSessionCookie);
        result.push_back('=');
        append_cookie_attributes(result, secure);
        result += "; Max-Age=0";
        return result;
    }

} // namespace circud
