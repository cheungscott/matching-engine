// tools/smoke.cpp — Framework-free Phase 0 smoke test.
//
// This exists so the compile/test loop is verifiable with ONLY g++ (no cmake,
// no Catch2). Once you install cmake, tests/test_types.cpp (Catch2) is the real
// suite and this file can retire.
//
// Build & run (from the project root):
//   g++ -std=c++23 -Wall -Wextra -Wconversion -Iinclude tools/smoke.cpp -o smoke && ./smoke
#include "me/types.hpp"

#include <cassert>
#include <iostream>
// NOTE: std::print (<print>) does NOT link on this MinGW g++ 15.2 build
// (undefined reference to std::__open_terminal / __write_to_terminal — a known
// libstdc++/MinGW gap). Using <iostream> here. This is exactly the "check
// compiler support" caveat from Blueprint §7; on this toolchain, prefer
// std::format + std::cout, or fmtlib, over std::print.

int main() {
    using namespace me;

    // Construct a resting limit buy order.
    Order o{
        .id = 1,
        .side = Side::Buy,
        .type = OrderType::Limit,
        .price = 100,
        .quantity = 50,
        .remaining = 50,
        .entry_seq = 1,
        .participant = 42,
    };

    assert(o.side == Side::Buy);
    assert(o.type == OrderType::Limit);
    assert(well_formed(o));            // remaining (50) <= quantity (50)

    // A partial fill keeps the order well-formed.
    o.remaining = 30;
    assert(well_formed(o));

    // A malformed order (remaining > quantity) is caught by the helper.
    Order bad = o;
    bad.remaining = 999;
    assert(!well_formed(bad));

    // NOTE on "deliberate misuse fails to COMPILE": because Side/OrderType are
    // `enum class`, a line like  `o.side = 0;`  or  `o.side = OrderType::Limit;`
    // is a compile error, not a silent bug. Uncomment to see it fail:
    //   o.side = 0;

    std::cout << "Phase 0 smoke test passed.\n";
    return 0;
}
