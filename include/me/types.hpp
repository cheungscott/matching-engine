// me/types.hpp — Core value types for the matching engine (Phase 0).
//
// Design source of truth: Obsidian vault
//   System/Internship/Matching-Engine-Design.md      (architecture — yours)
//   System/Internship/Matching-Engine-Blueprint.md   (build answer-key)
//
// Phase 0 scope: just the value types + enough to prove the compile/test loop.
// The matching logic is Phase 1+ and is YOURS to write — this header is the
// shared vocabulary those phases build on.
#pragma once

#include <cstdint>

namespace me {

// Scoped enums: passing a raw int or the wrong enum is a COMPILE error.
// This is Phase 0's "deliberate misuse fails" safety (see tools/smoke.cpp).
enum class Side      : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// Prices are INTEGER TICKS, never floating point: 10.1 + 0.2 != 10.3 in
// binary float, which would corrupt price-level keys. A tick's cash value is
// not the engine's concern.
//
// Phase 0 uses plain aliases (a defensible starting point — see Blueprint §3.5).
// A strong `struct Price { std::int64_t ticks; auto operator<=>() = default; };`
// wrapper is the upgrade to make price/quantity mixing a compile error; note it
// as an early Phase-1 option if the aliases bite.
using Price        = std::int64_t;   // ticks
using Quantity     = std::uint64_t;  // shares / contracts
using OrderId      = std::uint64_t;  // engine-assigned, monotonically increasing
using SeqNum       = std::uint64_t;  // event / log sequence number ("time")
using ParticipantId = std::uint64_t; // added in Phase 0 so self-trade prevention
                                     // (Blueprint §4.7) is a logic change later,
                                     // not a schema migration.

// A resting order. In later phases these live in an object pool and are linked
// intrusively via prev/next; Phase 0 only needs the fields.
struct Order {
    OrderId       id{};
    Side          side{};
    OrderType     type{};
    Price         price{};        // meaningless for Market; matcher asserts the convention
    Quantity      quantity{};     // original quantity
    Quantity      remaining{};    // decremented by fills; invariant: remaining <= quantity
    SeqNum        entry_seq{};    // arrival order == time priority
    ParticipantId participant{};  // for self-trade prevention (v1.5)
    // Intrusive links (Phase 3+): Order* prev/next — omitted until the pool exists.
};

// A trade print. Executes at the MAKER's price (Blueprint §4.2).
struct Trade {
    SeqNum   seq{};
    OrderId  maker_id{};
    OrderId  taker_id{};
    Price    price{};
    Quantity quantity{};
};

// Phase 0 invariant helper: a well-formed order never has remaining > quantity.
// Real invariant checking (all 7, see Blueprint §3.6) arrives with check_invariants()
// in Phase 4.
constexpr bool well_formed(const Order& o) noexcept {
    return o.remaining <= o.quantity;
}

} // namespace me
