// tests/mutation.hpp — deliberately break what the checkers are supposed to catch.
//
// D27. An audit neutered every clause of every invariant checker, one at a time, and
// re-ran the whole suite: 15 of 15 survived. check_invariants() accounts for a large
// share of the assertion count and NO test had ever planted a violation of any of the
// seven invariants. The suite states the rule out loud for the log property checker -
// "a checker that never fails proves nothing" - and had never applied it here.
//
// Several clauses guard state that is private and unreachable through the public API:
// the BBO cursors, the occupancy bitmap, the id-index counters, the pool's free list.
// So `Probe` is a friend of all three classes. It exists only in the test build, does
// nothing but write nonsense into those members, and every one of its methods exists
// because a specific `return false` had otherwise never been observed to fire.
#pragma once

#include "me/id_index.hpp"
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

    // D28. How many slots a lookup for `id` has to walk. Counted rather than timed,
    // so the clustering property can be pinned by a deterministic assertion instead
    // of a stopwatch. Identity hashing made this O(live entries); it must not return.
    static std::size_t probe_length(const IdIndex& idx, OrderId id) noexcept {
        std::size_t i = idx.home(id);
        std::size_t n = 0;
        for (; n < idx.table_.size(); ++n, i = (i + 1) & idx.mask_) {
            if (idx.table_[i].id == IdIndex::kEmpty) break;
            if (idx.table_[i].id == id)              break;
        }
        return n;
    }

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
