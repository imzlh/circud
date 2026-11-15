#include "test.hpp"

#include <circud/manifest.hpp>

#include <boost/json/serialize.hpp>

CIRCUD_TEST(manifest_accepts_minimal_package) {
    auto manifest = circud::parse_manifest(R"({
      "schema":1,"id":"io.example.demo","name":"demo","version":"1.2.3",
      "entry":"management.js","acceptPackageCalls":false
    })");
    circud::test::require(manifest.id == "io.example.demo");
    circud::test::require(!manifest.accept_package_calls);
}

CIRCUD_TEST(manifest_rejects_escape) {
    bool rejected = false;
    try {
        (void)circud::parse_manifest(R"({
          "schema":1,"id":"io.example.demo","name":"demo","version":"1.0.0",
          "entry":"../management.js"
        })");
    } catch (...) { rejected = true; }
    circud::test::require(rejected);
}

CIRCUD_TEST(manifest_rejects_unknown_fields) {
    bool root_rejected = false;
    try {
        (void)circud::parse_manifest(R"({
          "schema":1,"id":"io.example.demo","name":"Demo",
          "version":"1.0.0","acceptPackageCall":false
        })");
    } catch (const std::exception&) {
        root_rejected = true;
    }
    bool permission_rejected = false;
    try {
        (void)circud::parse_manifest(R"({
          "schema":1,"id":"io.example.demo","name":"Demo",
          "version":"1.0.0","permissions":{"network":true}
        })");
    } catch (const std::exception&) {
        permission_rejected = true;
    }
    circud::test::require(root_rejected,
                          "unknown top-level manifest field was accepted");
    circud::test::require(permission_rejected,
                          "unknown permission field was accepted");
}

CIRCUD_TEST(manifest_endpoint_prefix_defaults_to_package_namespace) {
    const auto manifest = circud::parse_manifest(R"({
      "schema":1,"id":"io.example.demo","name":"Demo","version":"1.0.0"
    })");
    circud::test::require(manifest.endpoint_prefix.empty());
    circud::test::require(circud::effective_endpoint_prefix(manifest) ==
        "/io.example.demo");

    const auto round_trip = circud::parse_manifest(
        boost::json::serialize(circud::manifest_to_json(manifest)));
    circud::test::require(round_trip.endpoint_prefix.empty());
    circud::test::require(circud::effective_endpoint_prefix(round_trip) ==
        "/io.example.demo");
}

CIRCUD_TEST(manifest_accepts_safe_custom_endpoint_prefix) {
    const auto manifest = circud::parse_manifest(R"({
      "schema":1,"id":"io.example.demo","name":"Demo","version":"1.0.0",
      "prefix":"/services/demo"
    })");
    circud::test::require(manifest.endpoint_prefix == "/services/demo");
    circud::test::require(circud::effective_endpoint_prefix(manifest) ==
        "/services/demo");

    const auto encoded = boost::json::serialize(
        circud::manifest_to_json(manifest));
    circud::test::require(encoded.find("\"prefix\":\"/services/demo\"") !=
        std::string::npos);
    circud::test::require(encoded.find("endpointPrefix") == std::string::npos);
    circud::test::require(encoded.find("endpoints") == std::string::npos);
    const auto round_trip = circud::parse_manifest(encoded);
    circud::test::require(round_trip.endpoint_prefix == "/services/demo");
}

CIRCUD_TEST(manifest_rejects_reserved_endpoint_prefixes) {
    const auto rejected = [](std::string_view prefix) {
        try {
            const auto document = std::string{
                R"({"schema":1,"id":"io.example.demo","name":"Demo","version":"1.0.0","prefix":")" } +
                std::string(prefix) + R"("})";
            (void)circud::parse_manifest(document);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    circud::test::require(rejected("/health"));
    circud::test::require(rejected("/rpc/internal"));
    circud::test::require(rejected("/api"));
    circud::test::require(rejected("/api/packages"));
    circud::test::require(rejected("/index.html"));
    circud::test::require(rejected("/app.js"));
    circud::test::require(rejected("/app.css/theme"));
    circud::test::require(rejected("/circud-api.d.ts"));
    circud::test::require(rejected("/assets/icons"));
    circud::test::require(rejected("/favicon.ico"));
    circud::test::require(rejected("/"));
    circud::test::require(rejected(""));
}

CIRCUD_TEST(manifest_rejects_invalid_endpoint_prefixes) {
    const auto rejected = [](std::string_view prefix) {
        try {
            const auto document = std::string{
                R"({"schema":1,"id":"io.example.demo","name":"Demo","version":"1.0.0","prefix":")" } +
                std::string(prefix) + R"("})";
            (void)circud::parse_manifest(document);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    circud::test::require(rejected("services/demo"));
    circud::test::require(rejected("/services/demo/"));
    circud::test::require(rejected("/services//demo"));
    circud::test::require(rejected("/services/./demo"));
    circud::test::require(rejected("/services/../demo"));
    circud::test::require(rejected("/services/%64emo"));
    circud::test::require(rejected("/services/demo?query"));
    circud::test::require(rejected("/services/demo#fragment"));
    circud::test::require(rejected(R"(/services\demo)"));

    std::string too_long = "/";
    too_long.append(256, 'a');
    circud::test::require(rejected(too_long));
}

CIRCUD_TEST(manifest_rejects_obsolete_endpoint_fields) {
    const auto rejected = [](std::string_view field) {
        try {
            const auto document = std::string{
                R"({"schema":1,"id":"io.example.demo","name":"Demo","version":"1.0.0",)" } +
                std::string(field) + "}";
            (void)circud::parse_manifest(document);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    circud::test::require(rejected(R"("endpointPrefix":"/demo")"));
    circud::test::require(rejected(R"("endpoints":[])"));
    circud::test::require(rejected(R"("prefix":123)"));
}
