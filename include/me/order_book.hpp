// me/order_book.hpp — tick-indexed levels + BBO cursors. Phase 1.
//
// ===========================================================================
//  Signatures from Blueprint §3.5; bodies are stubs until Phase 1.
// ===========================================================================
//
// WHY AN ARRAY AND NOT std::map:
//   - std::map is a red-black tree: O(log n), a heap allocation per node, and
//     pointer-chasing cache misses on every lookup.
//   - A flat sorted vector of levels costs O(n) on insert from element shifting.
//   - std::flat_map additionally INVALIDATES REFERENCES on insert/erase, which
//     is fatal for a design that stores handles into levels.
// A dense array indexed by tick is O(1) for everything price-related, contiguous,
// and prefetcher-friendly.
//
// THE COST ACCEPTED: a dense array assumes a BOUNDED TICK RANGE. A stock
// around tick 50,000 never visits tick 10^9, so a window is fine — as a stated
// assumption. Genuinely sparse or unbounded prices want a hash of
// levels plus a heap of occupied prices instead.
//
// THE OTHER COST: the best_bid_/best_ask_ CURSORS are maintained by hand. When the
// best level empties, something has to advance the cursor. Phase 1 may scan
// linearly; the graduation is an occupancy bitmap + std::countr_zero (<bit>).
// The simple one ships first.
//
// SEPARATION OF CONCERNS:
//   The book STORES. It never decides to match. add() has a PRECONDITION that
//   the order does not cross, and the engine guarantees that by matching first.
#pragma once

#include "me/price_level.hpp"
#include "me/types.hpp"

#include <optional>
#include <vector>

namespace me {

class OrderBook {
public:
    // Bounded tick window: [min_price, max_price] inclusive.
    OrderBook(Price min_price, Price max_price)
        : min_price_(min_price), max_price_(max_price) {
        // TODO: size levels_ and give each level its price.
    }

    // Rest an order. PRECONDITION: it does NOT cross the opposite side.
    void add(Order* /*o*/) {
        // TODO: index the level, push_back, update the cursor.
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        return std::nullopt;   // TODO
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        return std::nullopt;   // TODO
    }

    // nullptr if that side is empty. The ENGINE drives the fill loop, which is
    // why this leaks a mutable pointer — matching POLICY stays out of the book.
    [[nodiscard]] PriceLevel* best_level(Side /*side*/) {
        return nullptr;        // TODO
    }

    // Called after a fill or cancel empties the best level: advance the cursor.
    void on_level_emptied(Side /*side*/, Price /*price*/) {
        // TODO
    }

    [[nodiscard]] Price min_price() const noexcept { return min_price_; }
    [[nodiscard]] Price max_price() const noexcept { return max_price_; }

private:
    // levels_[price - min_price_]
    [[nodiscard]] std::size_t index_of(Price p) const noexcept {
        return static_cast<std::size_t>(p - min_price_);
    }

    std::vector<PriceLevel> levels_;
    Price min_price_ = 0;
    Price max_price_ = 0;

    // Cursors. A sentinel means "this side is empty", used consistently —
    // a half-initialised cursor is the classic source of a phantom BBO.
    Price best_bid_ = 0;
    Price best_ask_ = 0;
};

} // namespace me
