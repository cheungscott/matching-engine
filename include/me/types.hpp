// me/types.hpp — Core value types for the matching engine.
//
// Design source of truth: Obsidian vault
//   System/Internship/Matching-Engine-Design.md      (architecture — yours)
//   System/Internship/Matching-Engine-Blueprint.md   (build answer-key)
//
// The matching logic is Phase 1+ and is YOURS to write — this header is the
// shared vocabulary those phases build on.
#pragma once

#include <cstddef>
#include <cstdint>

namespace me {

// Scoped enums: passing a raw int or the wrong enum is a COMPILE error.
enum class Side      : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// Prices are INTEGER TICKS, never floating point: 10.1 + 0.2 != 10.3 in
// binary float, which would corrupt price-level keys. A tick's cash value is
// not the engine's concern.
//
// WIDTHS (see D7). Price and ParticipantId are 32-bit so Order lands on exactly
// one cache line. SeqNum is deliberately NOT narrowed: entry_seq IS time
// priority, and a wrapped sequence number would let two orders claim the same
// queue position — a correctness bug, not a performance one.
using Price         = std::int32_t;   // ticks · ±2.1e9 ticks, far beyond any real instrument
using Quantity      = std::uint64_t;  // shares / contracts
using OrderId       = std::uint64_t;  // engine-assigned, monotonically increasing
using SeqNum        = std::uint64_t;  // arrival / event number. NEVER narrow this.
using ParticipantId = std::uint32_t;  // present since Phase 0 so self-trade prevention
                                      // (Blueprint §4.7) is a logic change later,
                                      // not a schema migration.

// Cache line size on x86-64. Not std::hardware_destructive_interference_size:
// that is unavailable on several libstdc++ versions this project must build on,
// and it warns about ABI stability when it is available.
inline constexpr std::size_t kCacheLine = 64;

// A resting order. Lives in an ObjectPool slot while resting, and is linked
// INTRUSIVELY into a PriceLevel via prev/next — the links live inside the
// object rather than in separate node wrappers, so one order is one object at
// one address, and holding an Order* IS holding its queue position.
//
// LAYOUT (D7). Field order is Blueprint §3.5's, deliberately: identity first,
// container-only fields last. `side` and `type` are adjacent so they share one
// 8-byte slot, and the narrowed `price` drops into the padding they leave.
//
// alignas is doing real work here and is NOT decoration: fitting inside 64
// bytes is not enough. Objects packed end to end start at 0, 56, 112, ... and
// only the first begins on a cache-line boundary. Measured: a 56-byte Order
// straddled two lines in 3 of 4 slots. alignas(64) forces every Order to START
// on a boundary, so touching one is always exactly one memory fetch.
struct alignas(kCacheLine) Order {
    OrderId       id{};
    Side          side{};
    OrderType     type{};
    Price         price{};        // meaningless for Market; matcher asserts the convention
    Quantity      quantity{};     // original quantity
    Quantity      remaining{};    // decremented by fills; invariant: remaining <= quantity
    SeqNum        entry_seq{};    // arrival order == time priority
    ParticipantId participant{};  // for self-trade prevention (v1.5)
    Order*        prev{};         // intrusive links — the container does NOT own these
    Order*        next{};
};

// Both assertions matter, for different reasons.
//   size:  catches a future field quietly pushing Order to 128 bytes.
//   align: catches the mistake of assuming "it fits" is the same as "it starts
//          on a boundary". It is not — that is the whole point of D7.
static_assert(sizeof(Order)  == kCacheLine, "Order must be exactly one cache line");
static_assert(alignof(Order) == kCacheLine, "Order must START on a cache line, not merely fit");

// A trade print. Executes at the MAKER's price (Blueprint §4.2).
struct Trade {
    SeqNum   seq{};
    OrderId  maker_id{};
    OrderId  taker_id{};
    Price    price{};
    Quantity quantity{};
};

// A well-formed order never has remaining > quantity.
// Real invariant checking (all 7, see Blueprint §3.6) arrives with
// check_invariants() in Phase 4.
constexpr bool well_formed(const Order& o) noexcept {
    return o.remaining <= o.quantity;
}

} // namespace me
