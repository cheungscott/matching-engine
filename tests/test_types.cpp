// tests/test_types.cpp — Phase 0 Catch2 test (the real suite once CMake is set up).
//
// Mirrors tools/smoke.cpp but in the framework you'll grow. As you build
// Phase 1+, delete smoke.cpp and add cases here (Blueprint §9.1 has the checklist).
#include "me/types.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace me;

TEST_CASE("Order is well-formed when remaining <= quantity", "[types][phase0]") {
    Order o{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100, .quantity = 50, .remaining = 50,
        .entry_seq = 1, .participant = 42,
    };

    SECTION("freshly created order") {
        REQUIRE(o.side == Side::Buy);
        REQUIRE(o.type == OrderType::Limit);
        REQUIRE(well_formed(o));
    }

    SECTION("partial fill stays well-formed") {
        o.remaining = 30;
        REQUIRE(well_formed(o));
    }

    SECTION("remaining > quantity is caught") {
        o.remaining = 999;
        REQUIRE_FALSE(well_formed(o));
    }
}

// Compile-time proof that misuse is rejected: uncommenting either line below
// must FAIL to compile, because Side/OrderType are scoped enums.
//   static_assert([]{ Order x; x.side = 0;                  return true; }());
//   static_assert([]{ Order x; x.side = OrderType::Limit;   return true; }());
