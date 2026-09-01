// me/order_book.hpp — tick-indexed levels + BBO cursors.
//
// Storage only. The book NEVER decides to match: add() has a precondition that
// the order does not cross, and the Engine is what guarantees it.
//
// Rationale in SYSTEM-DESIGN.md D10. .
#pragma once

#include "me/price_level.hpp"
#include "me/types.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace me {

class OrderBook {
public:
    // Bounded tick window, inclusive. Out-of-range prices must be rejected by
    // the caller before they reach here (Blueprint §5.1 PriceOutOfRange).
    OrderBook(Price min_price, Price max_price)
        : levels_(checked_span(min_price, max_price)),
          min_price_(min_price),
          max_price_(max_price),
          best_bid_(min_price - 1),     // sentinel: below every real bid
          best_ask_(max_price + 1) {    // sentinel: above every real ask
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].set_price(min_price_ + static_cast<Price>(i));
        }
    }

    [[nodiscard]] bool in_range(Price p) const noexcept {
        return p >= min_price_ && p <= max_price_;
    }

    // Rest an order. PRECONDITION: it does not cross the opposite side.
    void add(Order* o) noexcept {
        assert(o != nullptr);
        assert(in_range(o->price) && "price outside the book's tick window");
        assert(!crosses(o->side, o->price) && "book must not decide to match; engine matches first");

        levels_[index_of(o->price)].push_back(o);

        if (o->side == Side::Buy) {
            if (o->price > best_bid_) best_bid_ = o->price;   // improved the bid
        } else {
            if (o->price < best_ask_) best_ask_ = o->price;   // improved the ask
        }
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        return has_bids() ? std::optional<Price>{best_bid_} : std::nullopt;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        return has_asks() ? std::optional<Price>{best_ask_} : std::nullopt;
    }

    // nullptr if that side is empty. Leaks a mutable pointer deliberately: the
    // ENGINE drives the fill loop, so matching policy stays out of the book.
    [[nodiscard]] PriceLevel* best_level(Side side) noexcept {
        if (side == Side::Buy) {
            return has_bids() ? &levels_[index_of(best_bid_)] : nullptr;
        }
        return has_asks() ? &levels_[index_of(best_ask_)] : nullptr;
    }

    // Call after a fill or cancel empties a level. Advances the cursor to the
    // next occupied price, or to the sentinel if that side is now empty.
    // No-op if `price` was not the best — a non-best level can empty via cancel.
    void on_level_emptied(Side side, Price price) noexcept {
        assert(in_range(price));
        assert(levels_[index_of(price)].empty() && "level is not actually empty");

        if (side == Side::Buy) {
            if (price != best_bid_) return;
            while (best_bid_ >= min_price_ && levels_[index_of(best_bid_)].empty()) {
                --best_bid_;                                  // linear scan down
            }
        } else {
            if (price != best_ask_) return;
            while (best_ask_ <= max_price_ && levels_[index_of(best_ask_)].empty()) {
                ++best_ask_;                                  // linear scan up
            }
        }
    }

    [[nodiscard]] Price min_price() const noexcept { return min_price_; }
    [[nodiscard]] Price max_price() const noexcept { return max_price_; }

    // Aggregate resting quantity at one price — this is L2 market data, and the
    // cached level total is what makes it O(1) rather than a walk.
    [[nodiscard]] Quantity depth_at(Price p) const noexcept {
        return in_range(p) ? levels_[index_of(p)].total_quantity() : 0;
    }

    // Does an incoming order at this price cross the opposite side?
    // "At or better" INCLUDES equal — the one-character bug (Blueprint §4.6).
    [[nodiscard]] bool crosses(Side side, Price price) const noexcept {
        if (side == Side::Buy)  return has_asks() && price >= best_ask_;
        return has_bids() && price <= best_bid_;
    }

    // O(range). Tests and Phase 4's check_invariants(), never the hot path.
    [[nodiscard]] bool is_consistent() const noexcept {
        // Invariant 1: both sides populated ⇒ best_bid < best_ask.
        if (has_bids() && has_asks() && best_bid_ >= best_ask_) return false;

        // Invariant 3: a cursor must point at a level that actually holds orders.
        if (has_bids() && levels_[index_of(best_bid_)].empty()) return false;
        if (has_asks() && levels_[index_of(best_ask_)].empty()) return false;

        // Nothing rests inside the spread, or a cursor is stale.
        const Price lo = has_bids() ? best_bid_ + 1 : min_price_;
        const Price hi = has_asks() ? best_ask_ - 1 : max_price_;
        for (Price p = lo; p <= hi; ++p) {
            if (!levels_[index_of(p)].empty()) return false;
        }

        for (const PriceLevel& lvl : levels_) {
            if (!lvl.is_consistent()) return false;
        }
        return true;
    }

private:
    static std::size_t checked_span(Price min_price, Price max_price) {
        if (min_price > max_price) {
            throw std::invalid_argument("OrderBook: min_price exceeds max_price");
        }
        // The sentinels are min-1 and max+1, so those must not overflow.
        if (min_price == std::numeric_limits<Price>::min() ||
            max_price == std::numeric_limits<Price>::max()) {
            throw std::invalid_argument("OrderBook: price window touches the limits of Price");
        }
        return static_cast<std::size_t>(max_price - min_price) + 1;
    }

    [[nodiscard]] std::size_t index_of(Price p) const noexcept {
        assert(in_range(p));
        return static_cast<std::size_t>(p - min_price_);
    }

    // A cursor sitting on its sentinel means that side holds nothing.
    [[nodiscard]] bool has_bids() const noexcept { return best_bid_ >= min_price_; }
    [[nodiscard]] bool has_asks() const noexcept { return best_ask_ <= max_price_; }

    std::vector<PriceLevel> levels_;      // indexed by (price - min_price_)
    Price min_price_ = 0;
    Price max_price_ = 0;
    Price best_bid_  = 0;                 // == min_price_ - 1 when no bids
    Price best_ask_  = 0;                 // == max_price_ + 1 when no asks
};

} // namespace me
