#include "test.hpp"

#include <circud/endpoint_gateway.hpp>

#include <stdexcept>
#include <vector>

namespace {

    circud::PackageManifest manifest(std::string id, std::string prefix = {}) {
        circud::PackageManifest value;
        value.id = std::move(id);
        value.name = value.id;
        value.version = "1.0.0";
        value.endpoint_prefix = std::move(prefix);
        return value;
    }

} // namespace

CIRCUD_TEST(endpoint_gateway_accepts_disjoint_scope_boundaries) {
    circud::EndpointGateway::validate_scopes({
        manifest("io.example.alpha", "/foo"),
        manifest("io.example.beta", "/foobar"),
        manifest("io.example.gamma", "/bar"),
    });
}

CIRCUD_TEST(endpoint_gateway_rejects_nested_or_duplicate_scopes) {
    const auto rejected = [](std::vector<circud::PackageManifest> manifests) {
        try {
            circud::EndpointGateway::validate_scopes(manifests);
            return false;
        }
        catch (const std::runtime_error&) {
            return true;
        }
    };
    circud::test::require(rejected({
        manifest("io.example.alpha", "/foo"),
        manifest("io.example.beta", "/foo/bar"),
    }), "nested endpoint scopes were accepted");
    circud::test::require(rejected({
        manifest("io.example.alpha", "/foo"),
        manifest("io.example.beta", "/foo"),
    }), "duplicate endpoint scopes were accepted");
}
