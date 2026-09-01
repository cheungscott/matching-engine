// me/order_book.hpp — tick-indexed levels + BBO cursors.
//
// Storage only. The book NEVER decides to match: add() has a precondition that
// the order does not cross, and the Engine is what guarantees it.
//
// Rationale in SYSTEM-DESIGN.md D10. .
#pragma once

#include "me/id_index.hpp"
#include "me/price_level.hpp"
#include "me/types.hpp"

#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace me {

class OrderBook {
public:
    // A tick window wider than this is a design error, not a runtime condition.
    static constexpr std::size_t kMaxLevels = 1u << 24;   // 16M levels

    // Bounded tick window, inclusive. Out-of-range prices must be rejected by
    // the caller before they reach here (Blueprint §5.1 PriceOutOfRange).
    //
    // `max_resting` sizes the id index and must be the pool's capacity: no more
    // orders can rest than the pool can hand out slots for (D19).
    OrderBook(Price min_price, Price max_price, std::size_t max_resting = 1 << 16)
        : levels_(checked_span(min_price, max_price)),
          by_id_(max_resting),
          min_price_(min_price),
          max_price_(max_price),
          best_bid_(min_price - 1),     // sentinel: below every real bid
          best_ask_(max_price + 1) {    // sentinel: above every real ask
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].set_price(min_price_ + static_cast<Price>(i));
        }
        occupied_.assign((levels_.size() + 63) / 64, 0);
    }

    [[nodiscard]] bool in_range(Price p) const noexcept {
        return p >= min_price_ && p <= max_price_;
    }

    // Rest an order. PRECONDITION: it does not cross the opposite side.
    void add(Order* o) {
        assert(o != nullptr);
        // UNCONDITIONAL (D19/F7). D8's rule is "a check preventing MEMORY
        // CORRUPTION is unconditional", and it was applied in ObjectPool and
        // nowhere else. Under NDEBUG an out-of-range price indexes levels_ with
        // an unchecked operator[] — an out-of-bounds WRITE, not a missed
        // diagnostic. Same class as ObjectPool's foreign-pointer release.
        if (!in_range(o->price)) {
            throw std::out_of_range("OrderBook::add: price outside the tick window");
        }
        assert(!crosses(o->side, o->price) && "book must not decide to match; engine matches first");

        const std::size_t li = index_of(o->price);
        levels_[li].push_back(o);
        occupied_[li >> 6] |= (std::uint64_t{1} << (li & 63));   // D20
        by_id_.insert(o->id, o);                 // D14: same step, always

        if (o->side == Side::Buy) {
            if (o->price > best_bid_) best_bid_ = o->price;   // improved the bid
        } else {
            if (o->price < best_ask_) best_ask_ = o->price;   // improved the ask
        }
    }

    // O(1) expected. nullptr if no such order is resting.
    [[nodiscard]] Order* find(OrderId id) const noexcept {
        return (id == 0) ? nullptr : by_id_.find(id);
    }

    // THE single removal path for the book. Unlink, drop the index entry, and
    // advance the cursor if the level emptied — all in one step, so no caller
    // can do two of the three (D14). The order is NOT returned to the pool here;
    // the book does not own the pool. Engine::retire() closes that loop.
    bool remove(Order* o) noexcept {
        assert(o != nullptr);
        // UNCONDITIONAL (D19/F7). Reports rather than aborts, following D8's
        // shape: the pool reports and the caller decides, and so does the book.
        if (o == nullptr || !in_range(o->price) || find(o->id) != o) {
            return false;
        }

        const Price price = o->price;
        const Side  side  = o->side;

        levels_[index_of(price)].unlink(o);
        by_id_.erase(o->id);

        if (levels_[index_of(price)].empty()) {
            on_level_emptied(side, price);
        }
        return true;
    }

    [[nodiscard]] std::size_t resting_count() const noexcept { return by_id_.size(); }

    // Sum of `remaining` over every resting order, walked from the intrusive
    // lists rather than summed from PriceLevel's cached totals — the
    // conservation property must not lean on the same cache is_consistent()
    // exists to validate, or the two checks would agree by construction.
    // O(resting), so this is a checkpoint check, not a per-operation one.
    [[nodiscard]] Quantity total_resting_quantity() const noexcept {
        Quantity total = 0;
        for (std::size_t li = next_occupied(0); li != kNoLevel; li = next_occupied(li + 1)) {
            for (const Order* o = levels_[li].front(); o != nullptr; o = o->next) {
                total += o->remaining;
            }
        }
        return total;
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
    // D20: the occupancy bitmap, not a linear scan.
    //
    // The scan it replaces was O(price range) inside a SINGLE operation, and the
    // audit measured 1,064 iterations in one call on this project's own
    // benchmark workload — a tail spike by construction. Blueprint §3.2 named
    // this graduation and D10 deferred it "until a profiler asks". It asked.
    //
    // One bit per level, so 64 empty levels are skipped per word, and
    // std::countr_zero/countl_zero find the next occupied bit in one
    // instruction. Worst case drops from range to range/64 words.
    void on_level_emptied(Side side, Price price) noexcept {
        assert(in_range(price));
        assert(levels_[index_of(price)].empty() && "level is not actually empty");

        const std::size_t li = index_of(price);
        occupied_[li >> 6] &= ~(std::uint64_t{1} << (li & 63));

        if (side == Side::Buy) {
            if (price != best_bid_) return;
            const std::size_t found = (li == 0) ? kNoLevel : prev_occupied(li - 1);
            best_bid_ = (found == kNoLevel) ? min_price_ - 1
                                            : min_price_ + static_cast<Price>(found);
        } else {
            if (price != best_ask_) return;
            const std::size_t found = next_occupied(li + 1);
            best_ask_ = (found == kNoLevel) ? max_price_ + 1
                                            : min_price_ + static_cast<Price>(found);
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

    // Blueprint §3.6 invariants 1-6. O(range + resting orders). Tests and
    // Phase 4's check_invariants(), never the hot path.
    [[nodiscard]] bool is_consistent() const noexcept {
        // 1: both sides populated ⇒ best_bid < best_ask.
        if (has_bids() && has_asks() && best_bid_ >= best_ask_) return false;

        // 3: a cursor must point at a level that actually holds orders.
        if (has_bids() && levels_[index_of(best_bid_)].empty()) return false;
        if (has_asks() && levels_[index_of(best_ask_)].empty()) return false;

        // Nothing rests inside the spread, or a cursor is stale.
        const Price lo = has_bids() ? best_bid_ + 1 : min_price_;
        const Price hi = has_asks() ? best_ask_ - 1 : max_price_;
        for (Price p = lo; p <= hi; ++p) {
            if (!levels_[index_of(p)].empty()) return false;
        }

        // 4 and 5, per level.
        for (const PriceLevel& lvl : levels_) {
            if (!lvl.is_consistent()) return false;
        }

        // 2 and 6: walk every resting order. Exactly one index entry each,
        // pointing at THIS order, and nothing rests with nothing left.
        std::size_t counted = 0;
        for (const PriceLevel& lvl : levels_) {
            for (const Order* o = lvl.front(); o != nullptr; o = o->next) {
                if (o->remaining == 0)            return false;   // 6
                if (o->price != lvl.price())      return false;
                if (find(o->id) != o)             return false;   // 2
                ++counted;
            }
        }
        // A stale entry — an id still mapped to an order that is in no level —
        // shows up here and nowhere else. The dangling-pointer detector.
        // count_live() walks the table so the counter is checked against the
        // structure rather than trusted.
        // D20: the bitmap is a second source of truth about which levels hold
        // orders. A stale bit sends a cursor to an empty level; a missing bit
        // makes a live level invisible. Neither is caught anywhere else.
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            const bool bit = (occupied_[i >> 6] >> (i & 63)) & 1u;
            if (bit == levels_[i].empty()) return false;
        }

        return counted == by_id_.count_live() && counted == by_id_.size();
    }

private:
    static constexpr std::size_t kNoLevel = ~std::size_t{0};

    // Lowest occupied level index >= from. C++20 <bit>, which Blueprint §7 marks
    // USE precisely for this.
    [[nodiscard]] std::size_t next_occupied(std::size_t from) const noexcept {
        if (from >= levels_.size()) return kNoLevel;
        std::size_t   w    = from >> 6;
        std::uint64_t bits = occupied_[w] & (~std::uint64_t{0} << (from & 63));
        for (;;) {
            if (bits != 0) {
                const std::size_t idx = (w << 6) + static_cast<std::size_t>(std::countr_zero(bits));
                return (idx < levels_.size()) ? idx : kNoLevel;
            }
            if (++w >= occupied_.size()) return kNoLevel;
            bits = occupied_[w];
        }
    }

    // Highest occupied level index <= from.
    [[nodiscard]] std::size_t prev_occupied(std::size_t from) const noexcept {
        if (levels_.empty()) return kNoLevel;
        if (from >= levels_.size()) from = levels_.size() - 1;
        std::size_t     w   = from >> 6;
        const unsigned  off = static_cast<unsigned>(from & 63);
        std::uint64_t   bits = occupied_[w] &
            ((off == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (off + 1)) - 1));
        for (;;) {
            if (bits != 0) {
                return (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(bits));
            }
            if (w == 0) return kNoLevel;
            --w;
            bits = occupied_[w];
        }
    }

    static std::size_t checked_span(Price min_price, Price max_price) {
        if (min_price > max_price) {
            throw std::invalid_argument("OrderBook: min_price exceeds max_price");
        }
        // The sentinels are min-1 and max+1, so those must not overflow.
        if (min_price == std::numeric_limits<Price>::min() ||
            max_price == std::numeric_limits<Price>::max()) {
            throw std::invalid_argument("OrderBook: price window touches the limits of Price");
        }
        // D19/F5: `max_price - min_price` is int32 arithmetic and overflows for
        // a wide window — UBSan-confirmed on (-2e9, 2e9). D10's reasoning covered
        // the sentinels and missed the span. Widen first; the guard cannot help
        // after the UB has already happened.
        const auto span = static_cast<std::int64_t>(max_price) -
                          static_cast<std::int64_t>(min_price) + 1;
        if (span > static_cast<std::int64_t>(kMaxLevels)) {
            throw std::invalid_argument("OrderBook: price window is too wide");
        }
        return static_cast<std::size_t>(span);
    }

    [[nodiscard]] std::size_t index_of(Price p) const noexcept {
        assert(in_range(p));
        return static_cast<std::size_t>(p - min_price_);
    }

    // A cursor sitting on its sentinel means that side holds nothing.
    [[nodiscard]] bool has_bids() const noexcept { return best_bid_ >= min_price_; }
    [[nodiscard]] bool has_asks() const noexcept { return best_ask_ <= max_price_; }

    // Declared BEFORE by_id_ on purpose: members initialise in DECLARATION order,
    // not member-init-list order, and checked_span() validates the price window.
    // Validate before allocating the index. -Wreorder catches this if it drifts.
    std::vector<PriceLevel>    levels_;      // indexed by (price - min_price_)
    std::vector<std::uint64_t> occupied_;    // D20: one bit per level

    // id -> node. THE gap Blueprint §3.4 names: a cancel carries only an id, so
    // without this, finding the order means scanning the book and the O(1)
    // cancel claim is silently false. Fixed capacity, allocates once — see D19
    // for why both previous attempts were wrong.
    IdIndex by_id_;
    Price min_price_ = 0;
    Price max_price_ = 0;
    Price best_bid_  = 0;                 // == min_price_ - 1 when no bids
    Price best_ask_  = 0;                 // == max_price_ + 1 when no asks
};

} // namespace me
