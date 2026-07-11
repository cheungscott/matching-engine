// me/types.hpp — Core value types for the matching engine.
//
// Scope: just the value types + enough to prove the compile/test loop.
// The shared vocabulary everything else builds on.
#pragma once

#include <cstdint>

namespace me {

// Scoped enums: passing a raw int or the wrong enum is a COMPILE error.
// This is the "deliberate misuse fails" safety (see tools/smoke.cpp).
enum class Side      : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// Prices are INTEGER TICKS, never floating point: 10.1 + 0.2 != 10.3 in
// binary float, which would corrupt price-level keys. A tick's cash value is
// not the engine's concern.
//
// Plain aliases for now (a defensible starting point).
// A strong `struct Price { std::int64_t ticks; auto operator<=>() = default; };`
// wrapper is the upgrade to make price/quantity mixing a compile error; an early
// option if the aliases cause bugs.
using Price        = std::int64_t;   // ticks
using Quantity     = std::uint64_t;  // shares / contracts
using OrderId      = std::uint64_t;  // engine-assigned, monotonically increasing
using SeqNum       = std::uint64_t;  // event / log sequence number ("time")
using ParticipantId = std::uint64_t; // present from the start so self-trade prevention
                                     // is a logic change later, not a schema migration.

// A resting order. Later these live in an object pool and are linked
// intrusively via prev/next; for now only the fields are needed.
struct Order {
    OrderId       id{};
    Side          side{};
    OrderType     type{};
    Price         price{};        // meaningless for Market; matcher asserts the convention
    Quantity      quantity{};     // original quantity
    Quantity      remaining{};    // decremented by fills; invariant: remaining <= quantity
    SeqNum        entry_seq{};    // arrival order == time priority
    ParticipantId participant{};  // for self-trade prevention (v1.5)
    // Intrusive links: Order* prev/next — omitted until the pool exists.
};

// A trade print. Executes at the MAKER's price.
struct Trade {
    SeqNum   seq{};
    OrderId  maker_id{};
    OrderId  taker_id{};
    Price    price{};
    Quantity quantity{};
};

// Invariant helper: a well-formed order never has remaining > quantity.
// Real invariant checking (all 7) arrives with check_invariants()
// later.
constexpr bool well_formed(const Order& o) noexcept {
    return o.remaining <= o.quantity;
}

} // namespace me
