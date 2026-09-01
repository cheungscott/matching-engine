// tests/mutation.hpp — deliberately break what the checkers are supposed to catch.
//
// A checker that never fails proves nothing. check_invariants() accounts for a large
// share of the suite's assertions, so each of its clauses needs a test that plants the
// violation it exists to catch and watches it fire; otherwise any clause could be
// deleted with no test noticing.
//
// Several clauses guard state that is private and unreachable through the public API:
// the BBO cursors, the occupancy bitmap, the id-index counter, a level's links and
// cached total, and the pool's free list. So `Probe` is a friend of OrderBook,
// PriceLevel, ObjectPool and IdIndex. It exists only in the test build, and each
// setter writes nonsense into one of those members so that a specific `return false`
// in a checker can be seen to fire.
#pragma once

#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/price_level.hpp"

namespace me {

struct Probe {
    // --- OrderBook cursors, bitmap and index ------------------------------
    static void set_best_bid(OrderBook& b, Price p) noexcept { b.best_bid_ = p; }
    static void set_best_ask(OrderBook& b, Price p) noexcept { b.best_ask_ = p; }

    static void flip_occupancy(OrderBook& b, Price p) noexcept {
        const std::size_t li = b.index_of(p);
        b.occupied_[li >> 6] ^= (std::uint64_t{1} << (li & 63));
    }

    static void bump_index_count(OrderBook& b) noexcept { ++b.by_id_.count_; }

    static PriceLevel& level_at(OrderBook& b, Price p) noexcept {
        return b.levels_[b.index_of(p)];
    }

    // --- PriceLevel links and cached total --------------------------------
    static void set_tail(PriceLevel& l, Order* o) noexcept { l.tail_ = o; }
    static void set_total(PriceLevel& l, Quantity q) noexcept { l.total_quantity_ = q; }

    // --- ObjectPool free list ---------------------------------------------
    template <class T>
    static void set_free_head(ObjectPool<T>& p, std::uint32_t i) noexcept { p.free_head_ = i; }
    template <class T>
    static void set_next_free(ObjectPool<T>& p, std::size_t i, std::uint32_t v) noexcept {
        p.next_free_[i] = v;
    }
};

} // namespace me
