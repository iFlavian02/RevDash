#include <catch2/catch_test_macros.hpp>
#include "revdash/core/types.hpp"

TEST_CASE("Core version and identity validation", "[core_types]") {
    REQUIRE(revdash::core::kApplicationName == "RevDash");
    REQUIRE(revdash::core::kApplicationVersion == "0.1.0");
}
