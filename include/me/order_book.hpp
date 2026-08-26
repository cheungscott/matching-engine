// me/order_book.hpp — tick-indexed levels + BBO cursors. Phase 1.
//
// ===========================================================================
//  THE BODIES ARE YOURS. Signatures from Blueprint §3.5.
// ===========================================================================
//
// WHY AN ARRAY AND NOT std::map (be able to reproduce this in an interview):
//   - std::map is a red-black tree: O(log n), a heap allocation per node, and
//     pointer-chasing cache misses on every lookup.
//   - A flat sorted vector of levels costs O(n) on insert from element shifting.
//   - std::flat_map additionally INVALIDATES REFERENCES on insert/erase, which
//     is fatal for a design that stores handles into levels.
// A dense array indexed by tick is O(1) for everything price-related, contiguous,
// and prefetcher-friendly.
//
// THE COST YOU ACCEPT: a dense array assumes a BOUNDED TICK RANGE. A stock
// around tick 50,000 never visits tick 10^9, so a window is fine — but say it
// out loud as an assumption. Genuinely sparse or unbounded prices want a hash of
// levels plus a heap of occupied prices instead.
//
// THE OTHER COST: you maintain best_bid_/best_ask_ CURSORS by hand. When the
// best level empties, something has to advance the cursor. Phase 1 may scan
// linearly; the graduation is an occupancy bitmap + std::countr_zero (<bit>).
// Know both, ship the simple one.
//
// SEPARATION OF CONCERNS — do not blur this:
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
        // TODO(you): size levels_ and give each level its price.
    }

    // Rest an order. PRECONDITION: it does NOT cross the opposite side.
    void add(Order* /*o*/) {
        // TODO(you): index the level, push_back, update the cursor.
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        return std::nullopt;   // TODO(you)
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        return std::nullopt;   // TODO(you)
    }

    // nullptr if that side is empty. The ENGINE drives the fill loop, which is
    // why this leaks a mutable pointer — matching POLICY stays out of the book.
    [[nodiscard]] PriceLevel* best_level(Side /*side*/) {
        return nullptr;        // TODO(you)
    }

    // Called after a fill or cancel empties the best level: advance the cursor.
    void on_level_emptied(Side /*side*/, Price /*price*/) {
        // TODO(you)
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

    // Cursors. Pick a sentinel meaning "this side is empty" and be consistent —
    // a half-initialised cursor is the classic source of a phantom BBO.
    Price best_bid_ = 0;
    Price best_ask_ = 0;
};

} // namespace me
