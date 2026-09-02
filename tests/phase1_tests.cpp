// tests/phase1_tests.cpp — engine acceptance suite, Phases 1-2.
//
// Filename is now a misnomer; rename when it starts to grate. Tags are the
// reliable filter: [phase1] [phase2] and [pool] [level] [book] [engine].
//
// Blueprint §11 accept criteria covered here:
//   Phase 1 — ObjectPool · PriceLevel intrusive list · OrderBook add + BBO
//             cursors · apply(NewOrder) for rest-on-empty and exact full fill
//   Phase 2 — partial fills, FIFO within a level, cached level sums
//
// Build (Catch2 arrives via CMake FetchContent; run from WSL/Linux):
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
//   cmake --build build && ctest --test-dir build --output-on-failure
//
// The Debug config is the one that counts: it carries ASan/UBSan. Green under a
// MinGW build verifies logic only; only the sanitized Linux build verifies that
// the pointer surgery did not corrupt memory.
//

#include "me/engine.hpp"
#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/price_level.hpp"
#include "me/types.hpp"

#include "naive_book.hpp"
#include "mutation.hpp"
#include "properties.hpp"
#include "scenario.hpp"
#include "shrink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <random>

using namespace me;

namespace {

// Build a resting order without going through the pool, for the container tests.
// D27. Two tests used to read
//     check_bbo(book.best_bid(), 101);
// which passes with ZERO assertions when the cursor is broken - the exact defect
// they exist to catch. Mutating the cursor update out of OrderBook::add made both
// report "assertions: - none -" and pass.
//
// Elsewhere the same comparison was written as a bare `CHECK(*best_ask() == ...)`,
// which is undefined behaviour on an empty optional and is diagnosed by NEITHER
// AddressSanitizer nor UndefinedBehaviorSanitizer.
//
// So every BBO comparison goes through here: REQUIRE the value exists (which aborts
// the test rather than reading it), then compare.
void check_bbo(const std::optional<Price>& actual, Price expected) {
    REQUIRE(actual.has_value());
    CHECK(*actual == expected);
}

Order make_order(OrderId id, Side side, Price price, Quantity qty, SeqNum seq) {
    Order o{};
    o.id          = id;
    o.side        = side;
    o.type        = OrderType::Limit;
    o.price       = price;
    o.quantity    = qty;
    o.remaining   = qty;
    o.entry_seq   = seq;
    o.participant = 1;
    return o;
}

constexpr Price kMin = 90;
constexpr Price kMax = 110;

} // namespace

// ===========================================================================
//  ObjectPool — Blueprint invariant 7 (pool discipline)
// ===========================================================================

TEST_CASE("pool_reports_capacity", "[phase1][pool]") {
    ObjectPool<Order> pool(8);
    CHECK(pool.capacity() == std::size_t{8});
    CHECK(pool.in_use() == std::size_t{0});
}

TEST_CASE("pool_acquire_returns_distinct_live_slots", "[phase1][pool]") {
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    CHECK(pool.in_use() == std::size_t{2});
}

TEST_CASE("pool_release_recycles_rather_than_leaking", "[phase1][pool]") {
    ObjectPool<Order> pool(2);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    CHECK(pool.in_use() == std::size_t{2});

    pool.release(a);
    CHECK(pool.in_use() == std::size_t{1});

    // A pool of 2 that has released one MUST be able to hand one out again.
    // If this returns nullptr you are leaking slots, not recycling them.
    Order* c = pool.acquire();
    CHECK(c != nullptr);
    CHECK(pool.in_use() == std::size_t{2});

    pool.release(b);
    pool.release(c);
    CHECK(pool.in_use() == std::size_t{0});
}

TEST_CASE("pool_slots_are_writable_after_acquire", "[phase1][pool]") {
    // If you poison on release, you MUST unpoison on acquire. Under ASan this
    // test is what catches a missing unpoison — without it, the write below
    // reports use-after-poison on a slot you legitimately own.
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    CHECK(a != nullptr);
    if (a != nullptr) {
        *a = make_order(1, Side::Buy, 100, 50, 1);
        CHECK(a->price == Price{100});
    }
    pool.release(a);

    Order* b = pool.acquire();       // very likely the same slot, recycled
    CHECK(b != nullptr);
    if (b != nullptr) {
        *b = make_order(2, Side::Sell, 105, 25, 2);
        CHECK(b->price == Price{105});
    }
}

// ---------------------------------------------------------------------------
//  Invariant 7, the half ASan is blind to.
//
//  Poisoning catches use-AFTER-release. It says nothing about releasing twice,
//  or releasing a pointer the pool never owned — those corrupt the LINK array,
//  not the payload. These tests exist because that was the gap: PR #1 asserted
//  both properties and tested neither, and an assert cannot be tested under
//  Catch2 at all. release() returning bool is what makes them observable.
// ---------------------------------------------------------------------------

TEST_CASE("pool_release_reports_success", "[phase1][pool]") {
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    REQUIRE(a != nullptr);
    CHECK(pool.release(a));                       // true == actually released
    CHECK(pool.in_use() == std::size_t{0});
}

TEST_CASE("pool_release_null_is_a_noop", "[phase1][pool]") {
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    REQUIRE(a != nullptr);
    CHECK_FALSE(pool.release(nullptr));
    CHECK(pool.in_use() == std::size_t{1});       // unchanged
    CHECK(pool.free_list_is_consistent());
}

TEST_CASE("pool_double_release_is_rejected_not_absorbed", "[phase1][pool]") {
    // The corruption this prevents, verified on the pre-fix code with -DNDEBUG:
    // the second release spliced the free list into a cycle, in_use_ underflowed
    // to SIZE_MAX, and the next two acquires returned THE SAME SLOT.
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    REQUIRE(a != nullptr);

    CHECK(pool.release(a));                       // first: genuine
    CHECK_FALSE(pool.release(a));                 // second: refused
    CHECK(pool.in_use() == std::size_t{0});       // no underflow
    CHECK(pool.available() == pool.capacity());
    CHECK(pool.free_list_is_consistent());        // no cycle

    // And the slot is still handed out exactly once afterwards.
    Order* x = pool.acquire();
    Order* y = pool.acquire();
    CHECK(x != y);
}

TEST_CASE("pool_release_rejects_a_foreign_pointer", "[phase1][pool]") {
    // Pre-fix with -DNDEBUG this was a wild WRITE ~16 GB out, after 64 bytes of
    // memory the pool does not own had already been poisoned.
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    REQUIRE(a != nullptr);

    Order stack_order = make_order(99, Side::Buy, 100, 10, 99);
    CHECK_FALSE(pool.release(&stack_order));
    CHECK(pool.in_use() == std::size_t{1});       // untouched
    CHECK(pool.free_list_is_consistent());
    CHECK(stack_order.id == OrderId{99});         // and not poisoned
}

TEST_CASE("pool_release_rejects_an_interior_pointer", "[phase1][pool]") {
    // A pointer 8 bytes into a valid slot passes a range check but is not an
    // element boundary. Pre-fix, `slot - base` on it was undefined behaviour:
    // g++ 11 produced garbage that was neither in range nor kNil, so the sentinel
    // check did not fire and release() SEGVd — in a Debug build with ASan, UBSan
    // and asserts all on. UBSan did not diagnose the subtraction.
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    REQUIRE(a != nullptr);

    auto* interior = reinterpret_cast<Order*>(reinterpret_cast<std::byte*>(a) + 8);
    CHECK_FALSE(pool.release(interior));
    CHECK(pool.in_use() == std::size_t{1});
    CHECK(pool.free_list_is_consistent());
}

TEST_CASE("pool_acquire_clears_inherited_state", "[phase1][pool]") {
    // D8. Harmless until types.hpp gained intrusive prev/next — after which a
    // push_back that reads a stale `next` splices a cycle into a PriceLevel, and
    // ASan cannot see it because those bytes are legitimately live after acquire.
    ObjectPool<Order> pool(1);                    // capacity 1 forces the same slot back

    Order* first = pool.acquire();
    REQUIRE(first != nullptr);
    *first = make_order(7, Side::Sell, 105, 42, 7);
    first->next = first;                          // the stale link that would bite
    first->prev = first;
    REQUIRE(pool.release(first));

    Order* second = pool.acquire();
    REQUIRE(second != nullptr);
    REQUIRE(second == first);                     // same slot, recycled
    CHECK(second->next == nullptr);
    CHECK(second->prev == nullptr);
    CHECK(second->id == OrderId{0});
    CHECK(second->remaining == Quantity{0});
}

TEST_CASE("pool_exhausts_then_recovers", "[phase1][pool]") {
    ObjectPool<Order> pool(2);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    CHECK(pool.exhausted());
    CHECK(pool.acquire() == nullptr);             // and does not consume anything
    CHECK(pool.in_use() == std::size_t{2});

    REQUIRE(pool.release(a));
    CHECK_FALSE(pool.exhausted());
    CHECK(pool.acquire() != nullptr);
    (void)b;
}

TEST_CASE("pool_free_list_survives_churn", "[phase1][pool]") {
    ObjectPool<Order> pool(8);
    std::vector<Order*> live;

    for (int round = 0; round < 200; ++round) {
        while (!pool.exhausted()) {
            Order* p = pool.acquire();
            REQUIRE(p != nullptr);
            live.push_back(p);
        }
        REQUIRE(pool.free_list_is_consistent());
        for (Order* p : live) REQUIRE(pool.release(p));
        live.clear();
        REQUIRE(pool.free_list_is_consistent());
        REQUIRE(pool.in_use() == std::size_t{0});
    }
}

TEST_CASE("pool_degenerate_capacities", "[phase1][pool]") {
    SECTION("capacity 0 is a valid, permanently exhausted pool") {
        ObjectPool<Order> pool(0);
        CHECK(pool.capacity() == std::size_t{0});
        CHECK(pool.exhausted());
        CHECK(pool.acquire() == nullptr);
        CHECK(pool.free_list_is_consistent());
    }
    SECTION("capacity 1 round-trips") {
        ObjectPool<Order> pool(1);
        Order* a = pool.acquire();
        REQUIRE(a != nullptr);
        CHECK(pool.acquire() == nullptr);
        REQUIRE(pool.release(a));
        CHECK(pool.acquire() == a);
        CHECK(pool.free_list_is_consistent());
    }
}

// ===========================================================================
//  PriceLevel — FIFO by arrival, O(1) unlink, cached total
// ===========================================================================

TEST_CASE("level_starts_empty", "[phase1][level]") {
    PriceLevel lvl(102);
    CHECK(lvl.empty());
    CHECK(lvl.front() == nullptr);
    CHECK(lvl.total_quantity() == Quantity{0});
    CHECK(lvl.price() == Price{102});
}

TEST_CASE("level_push_back_one", "[phase1][level]") {
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    lvl.push_back(&a);

    CHECK(!lvl.empty());
    CHECK(lvl.front() == &a);
    CHECK(lvl.total_quantity() == Quantity{100});
}

TEST_CASE("level_is_fifo_oldest_at_front", "[phase1][level]") {
    // A3 arrives before A4, so A3 fills first. This is time priority, and it is
    // structural: fills come off the head, so being at the head IS being first.
    PriceLevel lvl(102);
    Order a3 = make_order(3, Side::Sell, 102, 100, 3);
    Order a4 = make_order(4, Side::Sell, 102, 150, 4);
    lvl.push_back(&a3);
    lvl.push_back(&a4);

    CHECK(lvl.front() == &a3);
    CHECK(lvl.total_quantity() == Quantity{250});
}

TEST_CASE("level_unlink_head_promotes_the_next", "[phase1][level]") {
    PriceLevel lvl(102);
    Order a3 = make_order(3, Side::Sell, 102, 100, 3);
    Order a4 = make_order(4, Side::Sell, 102, 150, 4);
    lvl.push_back(&a3);
    lvl.push_back(&a4);

    lvl.unlink(&a3);
    CHECK(lvl.front() == &a4);
    CHECK(lvl.total_quantity() == Quantity{150});
    CHECK(!lvl.empty());
}

TEST_CASE("level_unlink_tail_keeps_head", "[phase1][level]") {
    PriceLevel lvl(102);
    Order a3 = make_order(3, Side::Sell, 102, 100, 3);
    Order a4 = make_order(4, Side::Sell, 102, 150, 4);
    lvl.push_back(&a3);
    lvl.push_back(&a4);

    lvl.unlink(&a4);
    CHECK(lvl.front() == &a3);
    CHECK(lvl.total_quantity() == Quantity{100});
}

TEST_CASE("level_unlink_middle_keeps_list_intact", "[phase1][level]") {
    // The case that produces dangling pointers. Both neighbours must be
    // re-linked to each other, in both directions.
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 10, 1);
    Order b = make_order(2, Side::Sell, 102, 20, 2);
    Order c = make_order(3, Side::Sell, 102, 30, 3);
    lvl.push_back(&a);
    lvl.push_back(&b);
    lvl.push_back(&c);

    lvl.unlink(&b);
    CHECK(lvl.total_quantity() == Quantity{40});
    CHECK(lvl.front() == &a);

    // Walk what is left: a then c, and nothing else.
    lvl.unlink(&a);
    CHECK(lvl.front() == &c);
    lvl.unlink(&c);
    CHECK(lvl.empty());
    CHECK(lvl.total_quantity() == Quantity{0});
}

TEST_CASE("level_unlink_only_element_empties_it", "[phase1][level]") {
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    lvl.push_back(&a);
    lvl.unlink(&a);

    CHECK(lvl.empty());
    CHECK(lvl.front() == nullptr);
    CHECK(lvl.total_quantity() == Quantity{0});
}

TEST_CASE("level_is_reusable_after_being_emptied", "[phase1][level]") {
    // An emptied level is not a dead level — the price will be quoted again.
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    lvl.push_back(&a);
    lvl.unlink(&a);

    Order b = make_order(2, Side::Sell, 102, 70, 2);
    lvl.push_back(&b);
    CHECK(lvl.front() == &b);
    CHECK(lvl.total_quantity() == Quantity{70});
}

// ===========================================================================
//  OrderBook — add and the BBO cursors
// ===========================================================================

TEST_CASE("book_starts_with_no_bbo", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
}

TEST_CASE("book_add_buy_sets_best_bid_only", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    Order b = make_order(1, Side::Buy, 101, 300, 1);
    book.add(&b);

    CHECK(book.best_bid().has_value());
    check_bbo(book.best_bid(), 101);
    CHECK(!book.best_ask().has_value());
}

TEST_CASE("book_best_bid_is_the_highest", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    Order lo = make_order(1, Side::Buy, 100, 500, 1);
    Order hi = make_order(2, Side::Buy, 101, 300, 2);
    book.add(&lo);
    book.add(&hi);
    check_bbo(book.best_bid(), 101);

    // Adding a WORSE bid must not move the cursor.
    Order worse = make_order(3, Side::Buy, 99, 100, 3);
    book.add(&worse);
    check_bbo(book.best_bid(), 101);
}

TEST_CASE("book_best_ask_is_the_lowest", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    Order hi = make_order(1, Side::Sell, 103, 200, 1);
    Order lo = make_order(2, Side::Sell, 102, 250, 2);
    book.add(&hi);
    book.add(&lo);
    check_bbo(book.best_ask(), 102);

    Order worse = make_order(3, Side::Sell, 104, 100, 3);
    book.add(&worse);
    check_bbo(book.best_ask(), 102);
}

TEST_CASE("book_best_level_exposes_the_right_queue", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    book.add(&a);

    PriceLevel* lvl = book.best_level(Side::Sell);
    CHECK(lvl != nullptr);
    if (lvl != nullptr) {
        CHECK(lvl->price() == Price{102});
        CHECK(lvl->front() == &a);
    }
    CHECK(book.best_level(Side::Buy) == nullptr);
}

TEST_CASE("book_stays_uncrossed_invariant_1", "[phase1][book]") {
    // Both sides populated ⇒ best_bid < best_ask, at every public boundary.
    // A crossed book means the engine missed a trade. That is a BUG, not a
    // market condition.
    OrderBook book(kMin, kMax);
    Order bid = make_order(1, Side::Buy, 101, 300, 1);
    Order ask = make_order(2, Side::Sell, 102, 250, 2);
    book.add(&bid);
    book.add(&ask);

    CHECK(book.best_bid().has_value());
    CHECK(book.best_ask().has_value());
    REQUIRE(book.best_bid().has_value());
    REQUIRE(book.best_ask().has_value());
    CHECK(*book.best_bid() < *book.best_ask());
}

// ===========================================================================
//  Engine::apply — match first, rest second
// ===========================================================================

TEST_CASE("engine_rests_an_order_on_an_empty_book", "[phase1][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 300, .participant = 1}, trades);

    CHECK(trades.size() == std::size_t{0});
    CHECK(eng.book().best_bid().has_value());
    check_bbo(eng.book().best_bid(), 101);
}

TEST_CASE("engine_does_not_match_when_it_does_not_cross", "[phase1][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 2}, trades);

    CHECK(trades.size() == std::size_t{0});
    check_bbo(eng.book().best_bid(), 101);
    check_bbo(eng.book().best_ask(), 102);
}

TEST_CASE("engine_exact_full_fill_at_one_price", "[phase1][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 2}, trades);

    CHECK(trades.size() == std::size_t{1});
    if (trades.size() == 1) {
        CHECK(trades[0].price == Price{102});
        CHECK(trades[0].quantity == Quantity{100});
    }
    // Both sides fully consumed: the book is empty again.
    CHECK(!eng.book().best_bid().has_value());
    CHECK(!eng.book().best_ask().has_value());
}

TEST_CASE("engine_prints_the_trade_at_the_makers_price", "[phase1][engine]") {
    // The taker is willing to pay 103. The resting ask is at 102. The trade
    // MUST print at 102 — the maker set the terms first. The taker receives
    // price improvement. Backwards here and every P&L number downstream is wrong.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 103, .quantity = 100, .participant = 2}, trades);

    CHECK(trades.size() == std::size_t{1});
    if (trades.size() == 1) CHECK(trades[0].price == Price{102});
}

TEST_CASE("engine_treats_equal_prices_as_crossing", "[phase1][engine]") {
    // THE ONE-CHARACTER BUG. An order priced exactly AT the opposite best must
    // trade, not rest. `<` instead of `<=` leaves an ask at 101 sitting beside
    // a bid at 101 — a locked book, which is a missed trade.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 2}, trades);

    CHECK(trades.size() == std::size_t{1});
    if (trades.size() == 1) CHECK(trades[0].price == Price{101});
    CHECK(!eng.book().best_bid().has_value());
    CHECK(!eng.book().best_ask().has_value());
}

TEST_CASE("engine_attributes_maker_and_taker", "[phase1][engine]") {
    // Falls out of matching for free — the resting side is ALWAYS the maker —
    // but the emitted event has to carry it, because fees, rebates and every
    // downstream analytic key off it.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId maker = eng.apply(
        NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                 .price = 102, .quantity = 100, .participant = 1}, trades);
    const OrderId taker = eng.apply(
        NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                 .price = 102, .quantity = 100, .participant = 2}, trades);

    CHECK(trades.size() == std::size_t{1});
    if (trades.size() == 1) {
        CHECK(trades[0].maker_id == maker);
        CHECK(trades[0].taker_id == taker);
    }
}

// ===========================================================================
//  PHASE 2 — partial fills, FIFO within a level, level sums
// ===========================================================================

TEST_CASE("level_reduce_front_keeps_position_and_total", "[phase2][level]") {
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    Order b = make_order(2, Side::Sell, 102, 150, 2);
    lvl.push_back(&a);
    lvl.push_back(&b);

    lvl.reduce_front(40);

    CHECK(lvl.front() == &a);                       // it did nothing to lose its place
    CHECK(a.remaining == Quantity{60});
    CHECK(lvl.total_quantity() == Quantity{210});   // 250 - 40, invariant 4
    CHECK(lvl.is_consistent());
}

TEST_CASE("engine_partial_fill_of_the_resting_order", "[phase2][engine]") {
    // Incoming is SMALLER than the resting order. The maker stays, shrunk, and
    // keeps its queue position.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId maker = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                             .price = 102, .quantity = 200, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 50, .participant = 2}, trades);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].price == Price{102});
    CHECK(trades[0].quantity == Quantity{50});
    CHECK(trades[0].maker_id == maker);

    REQUIRE(eng.book().best_ask().has_value());
    check_bbo(eng.book().best_ask(), 102);
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_partial_fill_of_the_incoming_order", "[phase2][engine]") {
    // Incoming is LARGER than the only resting order. It trades what it can and
    // rests the remainder at its own price.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 60, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 2}, trades);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == Quantity{60});

    CHECK_FALSE(eng.book().best_ask().has_value());       // ask side consumed
    REQUIRE(eng.book().best_bid().has_value());
    check_bbo(eng.book().best_bid(), 102);          // 40 left, now a bid
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_consumes_a_level_in_fifo_order", "[phase2][engine]") {
    // Two makers at one price. The oldest fills first and the trades say so.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId first  = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                              .price = 102, .quantity = 100, .participant = 1}, trades);
    const OrderId second = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                              .price = 102, .quantity = 150, .participant = 2}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 250, .participant = 3}, trades);

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].maker_id == first);          // arrived first, filled first
    CHECK(trades[0].quantity == Quantity{100});
    CHECK(trades[1].maker_id == second);
    CHECK(trades[1].quantity == Quantity{150});
    CHECK(trades[0].seq < trades[1].seq);        // and the log records that order

    CHECK_FALSE(eng.book().best_ask().has_value());
    CHECK_FALSE(eng.book().best_bid().has_value());
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_stops_mid_level_leaving_the_second_maker_partly_filled", "[phase2][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId first  = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                              .price = 102, .quantity = 100, .participant = 1}, trades);
    const OrderId second = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                              .price = 102, .quantity = 150, .participant = 2}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 180, .participant = 3}, trades);

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].maker_id == first);
    CHECK(trades[0].quantity == Quantity{100});
    CHECK(trades[1].maker_id == second);
    CHECK(trades[1].quantity == Quantity{80});   // 180 - 100

    REQUIRE(eng.book().best_ask().has_value());
    check_bbo(eng.book().best_ask(), 102);  // 70 of `second` still resting
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_pool_returns_every_fully_consumed_maker", "[phase2][engine]") {
    // Invariant 7 across a fill: a maker consumed to zero goes back to the pool.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    CHECK(eng.pool().in_use() == std::size_t{1});

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 2}, trades);

    CHECK(eng.pool().in_use() == std::size_t{0});   // maker returned, taker never rested
    CHECK(eng.pool().free_list_is_consistent());
}

// ===========================================================================
//  PHASE 3 — walk levels, market orders, and the differential oracle
// ===========================================================================

TEST_CASE("engine_walks_two_price_levels", "[phase3][engine]") {
    // The sweep from the Module 1 worked example: 250 resting at 102, 200 at
    // 103, and a buy for 300 at limit 103.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId a3 = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                          .price = 102, .quantity = 100, .participant = 1}, trades);
    const OrderId a4 = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                          .price = 102, .quantity = 150, .participant = 1}, trades);
    const OrderId a5 = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                          .price = 103, .quantity = 200, .participant = 1}, trades);

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 103, .quantity = 300, .participant = 2}, trades);

    REQUIRE(trades.size() == 3);
    CHECK(trades[0].maker_id == a3);  CHECK(trades[0].price == Price{102});
    CHECK(trades[0].quantity == Quantity{100});
    CHECK(trades[1].maker_id == a4);  CHECK(trades[1].price == Price{102});
    CHECK(trades[1].quantity == Quantity{150});
    CHECK(trades[2].maker_id == a5);  CHECK(trades[2].price == Price{103});
    CHECK(trades[2].quantity == Quantity{50});   // level 102 exhausted, cursor advanced

    REQUIRE(eng.book().best_ask().has_value());
    check_bbo(eng.book().best_ask(), 103);       // 150 of a5 left
    CHECK(eng.book().depth_at(102) == Quantity{0});
    CHECK(eng.book().depth_at(103) == Quantity{150});
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_price_improvement_across_the_sweep", "[phase3][engine]") {
    // Willing to pay 104, but every fill prints at the resting price.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 101, .quantity = 50, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 50, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 104, .quantity = 100, .participant = 2}, trades);

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].price == Price{101});    // NOT 104
    CHECK(trades[1].price == Price{102});
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_market_order_sweeps_and_never_rests", "[phase3][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 40, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 105, .quantity = 40, .participant = 1}, trades);

    // Wants 200, only 80 exists. Takes it all and cancels the rest.
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Market,
                       .price = 0, .quantity = 200, .participant = 2}, trades);

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].price == Price{102});
    CHECK(trades[1].price == Price{105});      // walked past the spread, any price
    CHECK_FALSE(eng.book().best_ask().has_value());
    CHECK_FALSE(eng.book().best_bid().has_value());   // the remainder did NOT rest
    CHECK(eng.book().is_consistent());
}

TEST_CASE("engine_market_order_into_an_empty_book", "[phase3][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId id = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Market,
                                          .price = 0, .quantity = 100, .participant = 1}, trades);

    CHECK(id != Engine::kRejected);          // accepted, then cancelled for no liquidity
    CHECK(trades.empty());
    CHECK_FALSE(eng.book().best_bid().has_value());
    CHECK(eng.pool().in_use() == std::size_t{0});
}

TEST_CASE("engine_boundary_at_or_better_includes_equal", "[phase3][engine]") {
    // The one-character bug, checked in both directions.
    Engine eng(kMin, kMax, 64);

    SECTION("sell into a bid at exactly the same price") {
        std::vector<Trade> trades;
        eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                           .price = 101, .quantity = 100, .participant = 1}, trades);
        eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                           .price = 101, .quantity = 100, .participant = 2}, trades);
        REQUIRE(trades.size() == 1);
        CHECK(trades[0].price == Price{101});
        CHECK_FALSE(eng.book().best_bid().has_value());
    }

    SECTION("one tick away does NOT cross") {
        std::vector<Trade> trades;
        eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                           .price = 100, .quantity = 100, .participant = 1}, trades);
        eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                           .price = 101, .quantity = 100, .participant = 2}, trades);
        CHECK(trades.empty());
        check_bbo(eng.book().best_bid(), 100);
        check_bbo(eng.book().best_ask(), 101);
        CHECK(eng.book().is_consistent());
    }
}

// ---------------------------------------------------------------------------
//  The differential test. Blueprint §9.3: highest value-per-line in the project.
//
//  Feed identical command streams to the real engine and to a deliberately
//  obvious std::map implementation, and compare after EVERY operation. Any
//  disagreement is a bug in one of them, and the naive one is the one you can
//  check by eye.
//
//  `seq` is excluded from the comparison: it is an event-numbering scheme, not
//  a matching outcome, and NaiveBook does not model the engine's counter.
// ---------------------------------------------------------------------------

TEST_CASE("differential_engine_matches_the_naive_book", "[phase3][oracle]") {
    constexpr Price kLo = 98;      // narrow band so orders actually cross
    constexpr Price kHi = 104;
    constexpr int   kOps = 2000;

    Engine            real(kMin, kMax, 8192);
    naive::NaiveBook  ref(kMin, kMax);

    std::mt19937 rng(20260901u);   // fixed seed: a failing run must be replayable
    std::uniform_int_distribution<int> price_of(kLo, kHi);
    std::uniform_int_distribution<int> qty_of(1, 200);
    std::uniform_int_distribution<int> roll(0, 99);

    for (int op = 0; op < kOps; ++op) {
        const int r = roll(rng);
        NewOrder cmd{
            .side        = (r % 2 == 0) ? Side::Buy : Side::Sell,
            .type        = (r < 8) ? OrderType::Market : OrderType::Limit,
            .price       = static_cast<Price>(price_of(rng)),
            .quantity    = static_cast<Quantity>(qty_of(rng)),
            .participant = 1,
        };

        std::vector<Trade> got;
        std::vector<Trade> want;
        const OrderId got_id  = real.apply(cmd, got);
        const OrderId want_id = ref.apply(cmd, want);

        INFO("diverged at operation " << op);
        REQUIRE(got_id == want_id);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            REQUIRE(got[i].maker_id == want[i].maker_id);
            REQUIRE(got[i].taker_id == want[i].taker_id);
            REQUIRE(got[i].price    == want[i].price);
            REQUIRE(got[i].quantity == want[i].quantity);
        }

        REQUIRE(real.book().best_bid() == ref.best_bid());
        REQUIRE(real.book().best_ask() == ref.best_ask());
        for (Price p = kMin; p <= kMax; ++p) {
            REQUIRE(real.book().depth_at(p) == ref.depth_at(p));
        }
        REQUIRE(real.book().is_consistent());
    }

    // The pool must not have leaked a slot across 2000 operations.
    REQUIRE(real.pool().free_list_is_consistent());
}

// ===========================================================================
//  PHASE 4 — cancel by id, the index, and check_invariants()
// ===========================================================================

TEST_CASE("cancel_removes_a_resting_order", "[phase4][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId id = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                          .price = 101, .quantity = 300, .participant = 1}, trades);
    REQUIRE(eng.book().best_bid().has_value());
    REQUIRE(eng.pool().in_use() == std::size_t{1});

    CHECK(eng.apply(Cancel{.id = id}));

    CHECK_FALSE(eng.book().best_bid().has_value());
    CHECK(eng.pool().in_use() == std::size_t{0});   // slot returned, invariant 7
    CHECK(eng.check_invariants());
}

TEST_CASE("cancel_of_an_unknown_id_is_routine", "[phase4][engine]") {
    // Not an error. A fill and a cancel legitimately race and the fill can win.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 300, .participant = 1}, trades);

    CHECK_FALSE(eng.apply(Cancel{.id = 9999}));
    CHECK(eng.pool().in_use() == std::size_t{1});   // untouched
    CHECK(eng.check_invariants());
}

TEST_CASE("cancel_after_the_order_already_filled", "[phase4][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId maker = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                             .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 2}, trades);
    REQUIRE(trades.size() == 1);

    // The order is gone, and its id must NOT still be in the index.
    CHECK_FALSE(eng.apply(Cancel{.id = maker}));
    CHECK(eng.check_invariants());
}

TEST_CASE("cancel_from_the_middle_of_a_level_keeps_fifo", "[phase4][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId a = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                         .price = 102, .quantity = 10, .participant = 1}, trades);
    const OrderId b = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                         .price = 102, .quantity = 20, .participant = 1}, trades);
    const OrderId c = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                         .price = 102, .quantity = 30, .participant = 1}, trades);

    CHECK(eng.apply(Cancel{.id = b}));                  // the middle one
    CHECK(eng.book().depth_at(102) == Quantity{40});    // 10 + 30
    CHECK(eng.check_invariants());

    // A and C still fill in arrival order.
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 40, .participant = 2}, trades);
    REQUIRE(trades.size() == 2);
    CHECK(trades[0].maker_id == a);
    CHECK(trades[1].maker_id == c);
    CHECK(eng.check_invariants());
}

TEST_CASE("cancel_that_empties_the_best_level_advances_the_cursor", "[phase4][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    const OrderId best = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                            .price = 102, .quantity = 10, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 105, .quantity = 10, .participant = 1}, trades);
    check_bbo(eng.book().best_ask(), 102);

    CHECK(eng.apply(Cancel{.id = best}));
    REQUIRE(eng.book().best_ask().has_value());
    check_bbo(eng.book().best_ask(), 105);    // cursor walked outward
    CHECK(eng.check_invariants());
}

TEST_CASE("cancel_of_a_non_best_level_leaves_the_cursor_alone", "[phase4][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 10, .participant = 1}, trades);
    const OrderId deep = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                            .price = 105, .quantity = 10, .participant = 1}, trades);

    CHECK(eng.apply(Cancel{.id = deep}));
    check_bbo(eng.book().best_ask(), 102);    // unmoved
    CHECK(eng.book().depth_at(105) == Quantity{0});
    CHECK(eng.check_invariants());
}

TEST_CASE("cancelling_everything_returns_every_slot", "[phase4][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;
    std::vector<OrderId> ids;

    for (int i = 0; i < 20; ++i) {
        ids.push_back(eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                         .price = static_cast<Price>(95 + (i % 5)),
                                         .quantity = 10, .participant = 1}, trades));
    }
    REQUIRE(eng.pool().in_use() == std::size_t{20});

    for (OrderId id : ids) CHECK(eng.apply(Cancel{.id = id}));

    CHECK(eng.pool().in_use() == std::size_t{0});
    CHECK_FALSE(eng.book().best_bid().has_value());
    CHECK(eng.check_invariants());
}

TEST_CASE("differential_with_cancels", "[phase4][oracle]") {
    // The oracle, now including cancels. Every removal path in the engine -
    // fill-to-zero and cancel - is exercised against an implementation that
    // finds orders by scanning, so it cannot share the index's bugs.
    constexpr Price kLo = 98;
    constexpr Price kHi = 104;
    constexpr int   kOps = 3000;

    Engine           real(kMin, kMax, 8192);
    naive::NaiveBook ref(kMin, kMax);

    std::mt19937 rng(20260902u);
    std::uniform_int_distribution<int> price_of(kLo, kHi);
    std::uniform_int_distribution<int> qty_of(1, 200);
    std::uniform_int_distribution<int> roll(0, 99);

    std::vector<OrderId> seen;     // ids ever accepted; many are long gone

    for (int op = 0; op < kOps; ++op) {
        const int r = roll(rng);
        INFO("diverged at operation " << op);

        if (r < 25 && !seen.empty()) {
            // Cancel something. Often already filled, which is the interesting
            // case: both books must agree it is unknown.
            std::uniform_int_distribution<std::size_t> pick(0, seen.size() - 1);
            const OrderId victim = seen[pick(rng)];
            REQUIRE(real.apply(Cancel{.id = victim}) == ref.cancel(victim));
        } else {
            NewOrder cmd{
                .side        = (r % 2 == 0) ? Side::Buy : Side::Sell,
                .type        = (r < 32) ? OrderType::Market : OrderType::Limit,
                .price       = static_cast<Price>(price_of(rng)),
                .quantity    = static_cast<Quantity>(qty_of(rng)),
                .participant = 1,
            };

            std::vector<Trade> got;
            std::vector<Trade> want;
            const OrderId got_id  = real.apply(cmd, got);
            const OrderId want_id = ref.apply(cmd, want);

            REQUIRE(got_id == want_id);
            REQUIRE(got.size() == want.size());
            for (std::size_t i = 0; i < got.size(); ++i) {
                REQUIRE(got[i].maker_id == want[i].maker_id);
                REQUIRE(got[i].taker_id == want[i].taker_id);
                REQUIRE(got[i].price    == want[i].price);
                REQUIRE(got[i].quantity == want[i].quantity);
            }
            if (got_id != Engine::kRejected) seen.push_back(got_id);
        }

        REQUIRE(real.book().best_bid() == ref.best_bid());
        REQUIRE(real.book().best_ask() == ref.best_ask());
        for (Price p = kMin; p <= kMax; ++p) {
            REQUIRE(real.book().depth_at(p) == ref.depth_at(p));
        }
        REQUIRE(real.check_invariants());       // all seven, after every operation
    }
}

// ===========================================================================
//  PHASE 6 — sequenced event log and replay
// ===========================================================================

namespace {

// A deterministic command stream. Fixed seed, so a failure is reproducible
// rather than a story about a run that once went wrong.
std::vector<scenario::Command> make_stream(unsigned seed, int ops) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price_of(98, 104);
    std::uniform_int_distribution<int> qty_of(1, 200);
    std::uniform_int_distribution<int> roll(0, 99);

    std::vector<scenario::Command> cmds;
    std::vector<OrderId>           limit_ids;   // ids that could plausibly still rest
    OrderId next_expected_id = 1;

    for (int i = 0; i < ops; ++i) {
        const int r = roll(rng);
        if (r < 20 && next_expected_id > 1) {
            // D27/1.13. This drew uniformly over EVERY id ever issued, so 92% of
            // cancels missed and `cancel, hit` was 7.8% of the stream — the path with
            // the most engine work in it (index lookup, unlink, index erase, pool
            // release, possible cursor advance) was the least exercised. That is the
            // same defect D21/F13 fixed in bench/latency.cpp, still living on the test
            // side, which is why fixing it there did not fix it here.
            //
            // Real cancels target recent quotes, so bias there; keep a minority of
            // long-range picks so the miss path stays covered too.
            // Target ids issued to LIMIT orders: a market order never rests, so a
            // cancel aimed at one is guaranteed to miss, and a quarter of this stream
            // is market orders. Bias to recent limits, keeping a minority of
            // long-range picks so the miss path stays covered.
            OrderId id = 0;
            if (!limit_ids.empty() && roll(rng) < 85) {
                const std::size_t window = std::min<std::size_t>(limit_ids.size(), 128);
                std::uniform_int_distribution<std::size_t> pick(limit_ids.size() - window,
                                                                limit_ids.size() - 1);
                id = limit_ids[pick(rng)];
            } else {
                std::uniform_int_distribution<unsigned long long> pick(1, next_expected_id - 1);
                id = static_cast<OrderId>(pick(rng));
            }
            cmds.emplace_back(Cancel{.id = id});
        } else {
            cmds.emplace_back(NewOrder{
                .side        = (r % 2 == 0) ? Side::Buy : Side::Sell,
                .type        = (r < 26) ? OrderType::Market : OrderType::Limit,
                .price       = static_cast<Price>(price_of(rng)),
                .quantity    = static_cast<Quantity>(qty_of(rng)),
                .participant = 1,
            });
            if (std::get<NewOrder>(cmds.back()).type == OrderType::Limit) {
                limit_ids.push_back(next_expected_id);
            }
            ++next_expected_id;
        }
    }
    return cmds;
}

} // namespace

TEST_CASE("the log's text format is pinned, field by field", "[phase6][replay]") {
    // D27/1.10. `grep 'ACC |TRD |CXL |REJ ' tests/` returned NOTHING: to_line was only
    // ever compared against itself, so the replay tests would stay green if the format
    // dropped fields entirely. Verified by mutation — removing price and quantity from
    // the TRD line left the whole suite passing. The log is the artefact the project
    // calls "the truth", so its shape is pinned here.
    CHECK(to_line(OrderAccepted{.seq = 1, .id = 2, .side = Side::Sell,
                                .type = OrderType::Limit, .price = 100, .quantity = 10})
          == "ACC 1 2 1 0 100 10");
    CHECK(to_line(OrderAccepted{.seq = 3, .id = 4, .side = Side::Buy,
                                .type = OrderType::Market, .price = 0, .quantity = 7})
          == "ACC 3 4 0 1 0 7");
    CHECK(to_line(TradeExecuted{.seq = 5, .maker_id = 2, .taker_id = 4,
                                .price = 100, .quantity = 7}) == "TRD 5 2 4 100 7");
    CHECK(to_line(OrderCancelled{.seq = 6, .id = 2,
                                 .reason = CancelReason::UserRequested}) == "CXL 6 2 0");
    CHECK(to_line(OrderRejected{.seq = 7, .reason = RejectReason::UnknownOrder})
          == "REJ 7 3");

    // Negative prices must survive the round trip: the tick window is signed.
    CHECK(to_line(TradeExecuted{.seq = 8, .maker_id = 1, .taker_id = 2,
                                .price = -5, .quantity = 1}) == "TRD 8 1 2 -5 1");
}

TEST_CASE("two market orders differing only in junk price log identically",
          "[phase6][replay]") {
    // D19 normalises a market order's meaningless price to 0 so that behaviourally
    // identical orders produce byte-identical logs. D27/1.10: no test ever fed two
    // such streams, so removing the canonicalisation left the suite green — the
    // replay tests compare a stream against ITSELF, which any consistent format
    // satisfies.
    auto log_with = [](Price junk) {
        Engine     eng(kMin, kMax, 16);
        VectorSink sink;
        eng.set_sink(&sink);
        std::vector<Trade> out;
        eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                           .price = 100, .quantity = 10, .participant = 1}, out);
        eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Market,
                           .price = junk, .quantity = 4, .participant = 2}, out);
        return to_log(sink.events());
    };
    CHECK(log_with(0) == log_with(107));
    CHECK(log_with(0) == log_with(-99));
}

TEST_CASE("events_are_emitted_for_every_outcome", "[phase6][events]") {
    Engine eng(kMin, kMax, 64);
    VectorSink sink;
    eng.set_sink(&sink);
    std::vector<Trade> trades;

    const OrderId maker = eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                                             .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 40, .participant = 2}, trades);
    eng.apply(Cancel{.id = maker});
    eng.apply(Cancel{.id = 9999});                                   // unknown
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 999, .quantity = 10, .participant = 3}, trades);  // out of range

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 100, .quantity = 0, .participant = 3}, trades);   // qty 0
    eng.apply(NewOrder{.side = Side::Buy, .type = static_cast<OrderType>(9),
                       .price = 100, .quantity = 5, .participant = 3}, trades);   // malformed

    const auto& ev = sink.events();
    REQUIRE(ev.size() == 8);
    CHECK(std::holds_alternative<OrderAccepted>(ev[0]));    // the maker
    CHECK(std::holds_alternative<OrderAccepted>(ev[1]));    // the taker
    CHECK(std::holds_alternative<TradeExecuted>(ev[2]));    // partial fill, maker stays
    CHECK(std::holds_alternative<OrderCancelled>(ev[3]));   // maker cancelled
    CHECK(std::holds_alternative<OrderRejected>(ev[4]));    // unknown order
    CHECK(std::holds_alternative<OrderRejected>(ev[5]));    // price out of range

    // A rejected order consumes a sequence number but NOT an order id: it never
    // existed as far as the book is concerned.
    CHECK(std::get<OrderRejected>(ev[4]).reason == RejectReason::UnknownOrder);
    CHECK(std::get<OrderRejected>(ev[5]).reason == RejectReason::PriceOutOfRange);
    CHECK(std::get<OrderAccepted>(ev[0]).id == maker);

    // D27/1.15 — InvalidQuantity was produced by NO test anywhere, and MalformedOrder
    // is new in D25.1. Neither is reachable from any fuzz stream, because every
    // generator draws valid quantities and in-window prices, so without these two
    // lines both branches were dead in every run the project has ever done.
    CHECK(std::get<OrderRejected>(ev[6]).reason == RejectReason::InvalidQuantity);
    CHECK(std::get<OrderRejected>(ev[7]).reason == RejectReason::MalformedOrder);

    // D27/R1 — the comment above claims a rejected order consumes a sequence number
    // but NOT an id, and nothing asserted it. Four rejections have now happened, so
    // the next accepted order must still be maker + 2.
    const OrderId after = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                             .price = 95, .quantity = 5, .participant = 4}, trades);
    CHECK(after == maker + 2);
}

TEST_CASE("market_remainder_is_cancelled_with_no_liquidity", "[phase6][events]") {
    Engine eng(kMin, kMax, 64);
    VectorSink sink;
    eng.set_sink(&sink);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Market,
                       .price = 0, .quantity = 100, .participant = 1}, trades);

    const auto& ev = sink.events();
    REQUIRE(ev.size() == 2);
    REQUIRE(std::holds_alternative<OrderCancelled>(ev[1]));
    CHECK(std::get<OrderCancelled>(ev[1]).reason == CancelReason::NoLiquidity);
}

TEST_CASE("sequence_numbers_never_go_backwards", "[phase6][events]") {
    const auto cmds = make_stream(31337u, 2000);
    Engine eng(kMin, kMax, 8192);
    VectorSink sink;
    eng.set_sink(&sink);

    std::vector<Trade> scratch;
    for (const auto& c : cmds) {
        scratch.clear();
        if (const auto* n = std::get_if<NewOrder>(&c)) eng.apply(*n, scratch);
        else                                           eng.apply(*std::get_if<Cancel>(&c));
    }

    SeqNum last = 0;
    for (const Event& e : sink.events()) {
        const SeqNum s = std::visit([](const auto& x) { return x.seq; }, e);
        REQUIRE(s > last);              // strictly increasing: it IS the order
        last = s;
    }
    CHECK(sink.events().size() > 2000);
}

TEST_CASE("replay_the_same_input_twice_gives_a_byte_identical_log", "[phase6][replay]") {
    // The whole claim of the design, in one assertion. ~10k commands.
    const auto cmds = make_stream(20260906u, 10000);

    const std::string first  = scenario::run(cmds, kMin, kMax, 8192);
    const std::string second = scenario::run(cmds, kMin, kMax, 8192);

    REQUIRE(first.size() > 10000);      // it actually did something
    REQUIRE(first == second);           // byte for byte, fresh engine each time
}

TEST_CASE("a_scenario_round_trips_through_text", "[phase6][replay]") {
    // The log is only useful as a regression net if it survives being written
    // out and read back. This is also the LOBSTER on-ramp.
    const auto cmds = make_stream(4242u, 3000);

    const std::string   text   = scenario::to_text(cmds);
    const auto          parsed = scenario::parse(text);

    REQUIRE(parsed.size() == cmds.size());
    REQUIRE(scenario::to_text(parsed) == text);          // text -> commands -> text

    // And the reconstructed commands produce the identical event log.
    REQUIRE(scenario::run(parsed, kMin, kMax, 8192) == scenario::run(cmds, kMin, kMax, 8192));
}

TEST_CASE("the_log_changes_when_behaviour_changes", "[phase6][replay]") {
    // A replay test that passes no matter what is worthless. One different
    // command must move the log, or the diff is not actually watching anything.
    auto cmds = make_stream(777u, 500);
    const std::string before = scenario::run(cmds, kMin, kMax, 8192);

    cmds.emplace_back(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                               .price = 103, .quantity = 55, .participant = 1});
    const std::string after = scenario::run(cmds, kMin, kMax, 8192);

    CHECK(before != after);
    CHECK(after.rfind(before, 0) == 0);   // append-only: the prefix is unchanged
}

// ===========================================================================
//  PHASE 7 — property tests and full oracle fuzz. THE GATE.
// ===========================================================================

namespace {

// Drive a command stream into an engine the caller owns. Conservation needs the
// BOOK alive alongside the log, which run_for_events cannot give it.
void feed(Engine& eng, const std::vector<scenario::Command>& cmds) {
    std::vector<Trade> scratch;
    for (const auto& c : cmds) {
        scratch.clear();
        if (const auto* n = std::get_if<NewOrder>(&c)) eng.apply(*n, scratch);
        else                                           eng.apply(*std::get_if<Cancel>(&c));
    }
}

// Run a stream and return its event log, for property checking.
std::vector<Event> run_for_events(const std::vector<scenario::Command>& cmds,
                                  std::size_t pool_capacity = 8192) {
    Engine     eng(kMin, kMax, pool_capacity);
    VectorSink sink;
    eng.set_sink(&sink);
    feed(eng, cmds);
    return sink.events();
}

} // namespace

TEST_CASE("a lookup's probe length does not grow with the book", "[phase7][audit]") {
    // D28, and this is the regression that matters most in the file.
    //
    // The id index hashed by IDENTITY, on the reasoning that engine ids are strictly
    // increasing so masking "distributes them perfectly AND keeps recently-issued ids
    // adjacent". Perfect adjacency is MAXIMAL CLUSTERING: every live entry sat in one
    // contiguous run, and linear probing stops at the first EMPTY slot, so a lookup
    // landing inside that run walked to the end of it. Measured at 241 MICROSECONDS
    // for a single miss at 320,000 resting orders, growing linearly with depth.
    //
    // Counted, not timed: a stopwatch in a test suite is flaky, and the property here
    // is structural. If someone reinstates an identity hash this fails immediately.
    for (const std::size_t live : {1'000u, 4'000u, 16'000u, 64'000u}) {
        IdIndex            idx(live * 2);
        std::vector<Order> orders(live);
        for (std::size_t i = 0; i < live; ++i) {
            orders[i].id = static_cast<OrderId>(i + 1);
            idx.insert(orders[i].id, &orders[i]);
        }

        std::size_t worst = 0;
        for (std::size_t k = 0; k < 512; ++k) {
            // Ids that ALIAS onto the front of the id range under a masking hash.
            // Nothing malformed about them; the engine issues ids like this itself
            // once next_id_ passes the table size.
            worst = std::max(worst, Probe::probe_length(
                idx, static_cast<OrderId>(idx.slot_count() * 4 + 1 + k)));
        }
        // The bound is the BLOCK SIZE (64 slots), not the number of live entries, and
        // that is the whole property: consecutive ids share a block so a run cannot
        // grow past one, plus a little spill. Under the old identity hash this was
        // ~live, i.e. 64,000 on the last iteration.
        INFO("live=" << live << " worst probe=" << worst);
        CHECK(worst <= 128);
    }
}

TEST_CASE("the fuzz generator actually exercises the cancel-hit path", "[phase7][audit]") {
    // D27/1.13. The generator drew cancel ids uniformly over every id ever issued, so
    // 92% of them missed: `cancel, hit` was 7.8% of the stream and the most expensive
    // path in the engine (index lookup, unlink, index erase, pool release, cursor
    // advance) was the least exercised. Nothing about a green run said so, which is
    // why it survived a fix to the SAME defect on the benchmark side (D21/F13).
    // Pinned here so it cannot drift back silently.
    Engine             eng(kMin, kMax, 4096);
    const auto         cmds = make_stream(20260907u, 20'000);
    std::vector<Trade> out;
    std::size_t        hits = 0, cancels = 0;

    for (const auto& c : cmds) {
        out.clear();
        if (const auto* n = std::get_if<NewOrder>(&c)) { eng.apply(*n, out); }
        else { ++cancels; hits += eng.apply(*std::get_if<Cancel>(&c)) ? 1u : 0u; }
    }

    REQUIRE(cancels > 1'000);
    INFO("cancel hit rate " << hits << "/" << cancels);

    // Measured 805/4046 = 19.9%, up from 7.8%. Not higher, and deliberately not
    // forced higher: prices are drawn from a 7-tick band so most limit orders cross
    // and fill immediately rather than resting. Widening the band would raise this
    // number by making the market stop crossing, which would buy a better statistic
    // by testing a less interesting book. The threshold sits below the measured value
    // with margin, so it catches a regression without pinning RNG behaviour exactly.
    CHECK(hits * 100 >= cancels * 15);
}

TEST_CASE("the guards that exist to prevent corruption are themselves tested",
          "[phase7][audit]") {
    // D27/R18. Every one of these throws was added by an audit to stop an
    // out-of-bounds write or an overflow, and not one of them had a test. A guard
    // nobody exercises is indistinguishable from a guard that does not work.
    SECTION("add() refuses a price outside the tick window") {
        OrderBook book(kMin, kMax, 8);
        Order     o = make_order(1, Side::Buy, kMax + 100, 10, 1);
        CHECK_THROWS_AS(book.add(&o), std::out_of_range);
        CHECK(book.resting_count() == 0);
    }
    SECTION("checked_span refuses an inverted window") {
        CHECK_THROWS_AS(OrderBook(200, 100, 8), std::invalid_argument);
    }
    SECTION("checked_span refuses a window touching the limits of Price") {
        CHECK_THROWS_AS(OrderBook(std::numeric_limits<Price>::min(), 0, 8),
                        std::invalid_argument);
        CHECK_THROWS_AS(OrderBook(0, std::numeric_limits<Price>::max(), 8),
                        std::invalid_argument);
    }
    SECTION("checked_span survives the span that used to overflow int32") {
        // D19/F5's case, "UBSan-confirmed on (-2e9, 2e9)", had no test. It must
        // throw a clean invalid_argument, not wrap into a small positive span.
        CHECK_THROWS_AS(OrderBook(-2'000'000'000, 2'000'000'000, 8), std::invalid_argument);
    }
    SECTION("the pool refuses a capacity beyond its index space") {
        CHECK_THROWS_AS(ObjectPool<Order>(std::size_t{1} << 40), std::length_error);
    }
    SECTION("max_trades_per_apply reports the pool's bound") {
        // F9's whole point, and it had zero callers and zero tests.
        Engine eng(kMin, kMax, 1024);
        CHECK(eng.max_trades_per_apply() == 1024);
    }
}

// ===========================================================================
//  D27 — planted violations for the INVARIANT checkers.
//
//  An audit neutered every `return false` in OrderBook::is_consistent,
//  PriceLevel::is_consistent and ObjectPool::free_list_is_consistent, one at a
//  time, and re-ran the whole suite. 15 of 15 survived. check_invariants() is
//  called tens of thousands of times per gate run and no test had ever planted a
//  violation of any of the seven invariants.
//
//  HONEST SCOPE: these prove the checker FIRES on each corruption. Where two
//  clauses overlap (a bad cursor is both "crossed" and "points at an empty
//  level") a test does not prove which one caught it, so it does not prove every
//  clause is individually load-bearing. It does mean no corruption below goes
//  unnoticed, which was not previously true of any of them.
// ===========================================================================

TEST_CASE("the book's consistency check catches a corrupted cursor", "[phase7][plants]") {
    OrderBook book(kMin, kMax, 16);
    Order b1 = make_order(1, Side::Buy,  100, 10, 1);
    Order b2 = make_order(2, Side::Buy,  101, 10, 2);
    Order a1 = make_order(3, Side::Sell, 105, 10, 3);
    book.add(&b1); book.add(&b2); book.add(&a1);
    REQUIRE(book.is_consistent());

    SECTION("bid cursor above the ask cursor — the book reads as crossed") {
        Probe::set_best_bid(book, 106);
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("bid cursor points at a level holding nothing") {
        Probe::set_best_bid(book, 103);
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("ask cursor points at a level holding nothing") {
        Probe::set_best_ask(book, 104);
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("a live level is left sitting inside the spread") {
        Probe::set_best_bid(book, 99);       // 100 and 101 still hold orders
        CHECK_FALSE(book.is_consistent());
    }
}

TEST_CASE("the book's consistency check catches a corrupted order", "[phase7][plants]") {
    OrderBook book(kMin, kMax, 16);
    Order b1 = make_order(1, Side::Buy, 100, 10, 1);
    Order b2 = make_order(2, Side::Buy, 100, 20, 2);
    book.add(&b1); book.add(&b2);
    REQUIRE(book.is_consistent());

    SECTION("an order rests with nothing left — invariant 6") {
        b2.remaining = 0;
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("an order's price disagrees with the level holding it") {
        b1.price = 101;
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("the index does not point at this order — invariant 2") {
        b2.id = 777;                         // index still maps the old id
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("the cached level total disagrees with the walk — invariant 4") {
        Probe::set_total(Probe::level_at(book, 100), 999);
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("time priority is broken — invariant 5") {
        b2.entry_seq = 0;                    // now <= its predecessor
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("a back-link is broken") {
        b2.prev = nullptr;
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("the tail pointer no longer terminates the walk") {
        Probe::set_tail(Probe::level_at(book, 100), &b1);
        CHECK_FALSE(book.is_consistent());
    }
}

TEST_CASE("the book's consistency check catches a corrupted bitmap or index",
          "[phase7][plants]") {
    // D20's bitmap clause carries the comment "neither is caught anywhere else",
    // and until now nothing had ever made it fire.
    OrderBook book(kMin, kMax, 16);
    Order b1 = make_order(1, Side::Buy, 100, 10, 1);
    book.add(&b1);
    REQUIRE(book.is_consistent());

    SECTION("a stale bit marks an empty level as occupied") {
        Probe::flip_occupancy(book, 103);
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("a missing bit makes a live level invisible") {
        Probe::flip_occupancy(book, 100);
        CHECK_FALSE(book.is_consistent());
    }
    SECTION("the index counter drifts from the table") {
        Probe::bump_index_count(book);
        CHECK_FALSE(book.is_consistent());
    }
}

TEST_CASE("a level's consistency check catches a broken list", "[phase7][plants]") {
    PriceLevel lvl;
    lvl.set_price(100);
    Order o1 = make_order(1, Side::Buy, 100, 10, 1);
    Order o2 = make_order(2, Side::Buy, 100, 20, 2);
    lvl.push_back(&o1); lvl.push_back(&o2);
    REQUIRE(lvl.is_consistent());

    SECTION("head set, tail cleared") {
        Probe::set_tail(lvl, nullptr);
        CHECK_FALSE(lvl.is_consistent());
    }
    SECTION("the head has a predecessor") {
        o1.prev = &o2;
        CHECK_FALSE(lvl.is_consistent());
    }
    SECTION("the cached total is wrong") {
        Probe::set_total(lvl, 31);
        CHECK_FALSE(lvl.is_consistent());
    }
    SECTION("the tail does not terminate the walk") {
        Probe::set_tail(lvl, &o1);
        CHECK_FALSE(lvl.is_consistent());
    }
}

TEST_CASE("the pool's free-list check catches a corrupted list", "[phase7][plants]") {
    ObjectPool<Order> pool(8);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(pool.free_list_is_consistent());

    SECTION("the head index is out of range") {
        Probe::set_free_head(pool, 9999);
        CHECK_FALSE(pool.free_list_is_consistent());
    }
    SECTION("the free list walks into a slot that is handed out") {
        // Slot 0 is `a`, which is in use; splicing it into the free list is the
        // corruption that would let two callers hold the same Order.
        Probe::set_free_head(pool, 0);
        CHECK_FALSE(pool.free_list_is_consistent());
    }
    SECTION("the free list contains a cycle") {
        Probe::set_free_head(pool, 2);
        Probe::set_next_free(pool, 2, 2);      // points at itself
        CHECK_FALSE(pool.free_list_is_consistent());
    }
    SECTION("the free list is shorter than available() claims") {
        Probe::set_free_head(pool, std::numeric_limits<std::uint32_t>::max());
        CHECK_FALSE(pool.free_list_is_consistent());
    }
}

// ===========================================================================
//  D27 — a planted violation for EVERY branch of the log checkers.
//
//  Mutation testing found that 19 of 24 fail() branches could be deleted without
//  any test noticing, and that all four conservation plants tripped the SAME
//  branch while their comment claimed "one plant per check it makes".
//
//  So every plant below asserts WHICH branch fired, not merely that something
//  did. Checking only `!ok` is what let four plants masquerade as four checks.
// ===========================================================================

namespace {

Event acc(SeqNum s, OrderId id, Side side, OrderType type, Price p, Quantity q) {
    return OrderAccepted{.seq = s, .id = id, .side = side, .type = type,
                         .price = p, .quantity = q};
}
Event trd(SeqNum s, OrderId maker, OrderId taker, Price p, Quantity q) {
    return TradeExecuted{.seq = s, .maker_id = maker, .taker_id = taker,
                         .price = p, .quantity = q};
}
Event cxl(SeqNum s, OrderId id) {
    return OrderCancelled{.seq = s, .id = id, .reason = CancelReason::UserRequested};
}

// The point of the whole section: name the branch you expect.
void expect_check(const std::vector<Event>& log, const char* fragment) {
    const auto v = props::check(log, kMin, kMax);
    INFO("expected a violation containing: " << fragment);
    INFO("actual: " << (v.ok ? std::string("(no violation at all)") : v.why));
    REQUIRE_FALSE(v.ok);
    REQUIRE(v.why.find(fragment) != std::string::npos);
}

void expect_fold(const std::vector<Event>& log, const char* fragment) {
    props::Ledger  led;
    const auto     v = props::fold_ledger(log, led);
    INFO("expected a fold violation containing: " << fragment);
    INFO("actual: " << (v.ok ? std::string("(no violation at all)") : v.why));
    REQUIRE_FALSE(v.ok);
    REQUIRE(v.why.find(fragment) != std::string::npos);
}

} // namespace

TEST_CASE("every acceptance-checking branch has a planted violation", "[phase7][plants]") {
    SECTION("order id reused") {
        expect_check({acc(1, 1, Side::Buy, OrderType::Limit, 100, 10),
                      acc(2, 1, Side::Buy, OrderType::Limit, 101, 10)}, "order id reused");
    }
    SECTION("accepted a zero-quantity order") {
        expect_check({acc(1, 1, Side::Buy, OrderType::Limit, 100, 0)},
                     "accepted a zero-quantity order");
    }
}

TEST_CASE("every trade-legality branch has a planted violation", "[phase7][plants]") {
    // A well-formed pair to corrupt: maker sells 10 at 100, taker buys 10 at 100.
    const Event mk = acc(1, 1, Side::Sell, OrderType::Limit, 100, 10);
    const Event tk = acc(2, 2, Side::Buy,  OrderType::Limit, 100, 10);

    SECTION("zero-quantity trade") {
        expect_check({mk, tk, trd(3, 1, 2, 100, 0)}, "zero-quantity trade");
    }
    SECTION("trade outside the tick window") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Limit, 100, 10), tk,
                      trd(3, 1, 2, kMax + 50, 5)}, "trade outside the tick window");
    }
    SECTION("order traded with itself") {
        expect_check({mk, tk, trd(3, 1, 1, 100, 5)}, "order traded with itself");
    }
    SECTION("trade with an unaccepted maker") {
        expect_check({mk, tk, trd(3, 999, 2, 100, 5)}, "unaccepted maker");
    }
    SECTION("trade with an unaccepted taker") {
        expect_check({mk, tk, trd(3, 1, 999, 100, 5)}, "unaccepted taker");
    }
    SECTION("both sides of a trade agree") {
        expect_check({acc(1, 1, Side::Buy, OrderType::Limit, 100, 10), tk,
                      trd(3, 1, 2, 100, 5)}, "both sides of a trade agree");
    }
    SECTION("a market order rested as maker") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Market, 0, 10), tk,
                      trd(3, 1, 2, 100, 5)}, "market order rested as maker");
    }
    SECTION("trade not at the maker's price") {
        expect_check({mk, tk, trd(3, 1, 2, 101, 5)}, "not at the maker's price");
    }
    SECTION("buyer paid more than its limit") {
        // Taker will only pay 99; the maker rests at 100, so any print is too dear.
        expect_check({mk, acc(2, 2, Side::Buy, OrderType::Limit, 99, 10),
                      trd(3, 1, 2, 100, 5)}, "buyer paid more than its limit");
    }
    SECTION("seller received less than its limit") {
        expect_check({acc(1, 1, Side::Buy, OrderType::Limit, 100, 10),
                      acc(2, 2, Side::Sell, OrderType::Limit, 101, 10),
                      trd(3, 1, 2, 100, 5)}, "seller received less than its limit");
    }
}

TEST_CASE("both price-priority branches have a planted violation", "[phase7][plants]") {
    // D27's new property. A taker sweeps best-first, so its fills can only get worse
    // for it. A taker that trades at 101 and THEN at 100 skipped the better level.
    SECTION("a buy taker's fills improved") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Limit, 100, 10),
                      acc(2, 2, Side::Sell, OrderType::Limit, 101, 10),
                      acc(3, 3, Side::Buy,  OrderType::Limit, 105, 20),
                      trd(4, 2, 3, 101, 10),      // took the worse level first
                      trd(5, 1, 3, 100, 10)},     // then the better one
                     "buy taker's fills improved");
    }
    SECTION("a sell taker's fills improved") {
        expect_check({acc(1, 1, Side::Buy, OrderType::Limit, 101, 10),
                      acc(2, 2, Side::Buy, OrderType::Limit, 100, 10),
                      acc(3, 3, Side::Sell, OrderType::Limit, 95, 20),
                      trd(4, 2, 3, 100, 10),
                      trd(5, 1, 3, 101, 10)},
                     "sell taker's fills improved");
    }
}

TEST_CASE("every remaining check() branch has a planted violation", "[phase7][plants]") {
    SECTION("a cancelled order traded again") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Limit, 100, 10),
                      acc(2, 2, Side::Buy,  OrderType::Limit, 100, 10),
                      cxl(3, 1),
                      trd(4, 1, 2, 100, 5)}, "cancelled order traded again as maker");
    }
    SECTION("a cancelled order traded again, as the taker") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Limit, 100, 10),
                      acc(2, 2, Side::Buy,  OrderType::Limit, 100, 10),
                      cxl(3, 2),
                      trd(4, 1, 2, 100, 5)}, "cancelled order traded again as taker");
    }
    SECTION("maker filled beyond its original quantity") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Limit, 100, 5),
                      acc(2, 2, Side::Buy,  OrderType::Limit, 100, 10),
                      trd(3, 1, 2, 100, 10)}, "maker filled beyond");
    }
    SECTION("taker filled beyond its original quantity") {
        expect_check({acc(1, 1, Side::Sell, OrderType::Limit, 100, 10),
                      acc(2, 2, Side::Buy,  OrderType::Limit, 100, 5),
                      trd(3, 1, 2, 100, 10)}, "taker filled beyond");
    }
}

TEST_CASE("every fold_ledger branch has a planted violation", "[phase7][plants]") {
    // These are unreachable through props::check — it catches most of them earlier via
    // a different rule — so they are planted against the fold directly. Without this,
    // three branches of the conservation ledger had never been shown to fire.
    SECTION("fill against an order the log shows as not live") {
        expect_fold({acc(1, 1, Side::Sell, OrderType::Limit, 100, 5),
                     acc(2, 2, Side::Buy,  OrderType::Limit, 100, 5),
                     trd(3, 1, 2, 100, 5),        // both now fully filled, so both gone
                     trd(4, 1, 2, 100, 1)}, "not live");
    }
    SECTION("fill exceeds the order's outstanding quantity") {
        expect_fold({acc(1, 1, Side::Sell, OrderType::Limit, 100, 10),
                     acc(2, 2, Side::Buy,  OrderType::Limit, 100, 10),
                     trd(3, 1, 2, 100, 4),
                     trd(4, 1, 2, 100, 9)}, "exceeds the order's outstanding");
    }
    SECTION("cancel of an order the log shows as not live") {
        expect_fold({acc(1, 1, Side::Buy, OrderType::Limit, 100, 10),
                     cxl(2, 1),
                     cxl(3, 1)}, "cancel of an order the log shows as not live");
    }
}

TEST_CASE("every conservation branch that CAN fire has a planted violation",
          "[phase7][plants]") {
    // A real book with two resting orders and no trades, so the log can be corrupted
    // one field at a time without the fold rejecting it for an unrelated reason.
    Engine     eng(kMin, kMax, 64);
    VectorSink sink;
    eng.set_sink(&sink);
    std::vector<Trade> out;
    const OrderId a = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                         .price = 100, .quantity = 10, .participant = 1}, out);
    const OrderId b = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                         .price = 101, .quantity = 20, .participant = 1}, out);
    REQUIRE(a != Engine::kRejected);
    REQUIRE(b != Engine::kRejected);

    const std::vector<Event> clean = sink.events();
    REQUIRE(props::check_conservation(clean, eng.book()).ok);

    auto expect_cons = [&](const std::vector<Event>& log, const char* fragment) {
        const auto v = props::check_conservation(log, eng.book());
        INFO("expected: " << fragment << " | actual: "
                          << (v.ok ? std::string("(no violation)") : v.why));
        REQUIRE_FALSE(v.ok);
        REQUIRE(v.why.find(fragment) != std::string::npos);
    };

    SECTION("the counts disagree") {
        auto bad = clean;
        bad.push_back(acc(99, 12345, Side::Buy, OrderType::Limit, 100, 5));
        expect_cons(bad, "the log accounts for");
    }
    SECTION("the log rests an id the book never held") {
        // Count still matches: one id is SWAPPED, not added.
        auto bad = clean;
        std::get<OrderAccepted>(bad[1]).id = 4242;
        expect_cons(bad, "the book has no such order");
    }
    SECTION("the book holds a different remaining than the log implies") {
        auto bad = clean;
        std::get<OrderAccepted>(bad[1]).quantity += 7;
        expect_cons(bad, "the log implies");
    }
}

// ===========================================================================
//  D25 — regressions for the pre-ship audit. One per demonstrated defect.
//
//  Every one of these FAILED before its fix. They are here because the previous
//  audit's fixes introduced four of the nine, so "fixed" without "pinned" is how
//  this set got written in the first place.
// ===========================================================================

TEST_CASE("an OrderType outside the enumerators is rejected, not executed", "[phase7][audit]") {
    // D25.1. This was a null-pointer WRITE under NDEBUG: the value reserved no pool
    // slot, skipped price validation, was not Market, and still reached the resting
    // path. Reachable from scenario.hpp, which casts an integer straight off a log.
    Engine     eng(kMin, kMax, 64);
    VectorSink sink;
    eng.set_sink(&sink);
    std::vector<Trade> out;

    const OrderId id = eng.apply(NewOrder{.side = Side::Buy,
                                          .type = static_cast<OrderType>(2),
                                          .price = 100, .quantity = 10, .participant = 1}, out);

    CHECK(id == Engine::kRejected);
    CHECK(out.empty());
    CHECK(eng.book().resting_count() == 0);
    CHECK(eng.pool().in_use() == 0);
    REQUIRE(eng.check_invariants());
    REQUIRE(sink.events().size() == 1);
    const auto* r = std::get_if<OrderRejected>(&sink.events()[0]);
    REQUIRE(r != nullptr);
    CHECK(r->reason == RejectReason::MalformedOrder);
}

TEST_CASE("a Side outside the enumerators is rejected", "[phase7][audit]") {
    // D25.1. Less dangerous than the OrderType hole — every comparison is `== Buy`, so
    // a stray value behaves consistently as Sell — but it still writes junk into the
    // log, and the log is the artefact the replay test proves byte-identical.
    Engine             eng(kMin, kMax, 64);
    std::vector<Trade> out;
    CHECK(eng.apply(NewOrder{.side = static_cast<Side>(7), .type = OrderType::Limit,
                             .price = 100, .quantity = 10, .participant = 1}, out)
          == Engine::kRejected);
    CHECK(eng.book().resting_count() == 0);
}

TEST_CASE("a quantity above the cap is rejected before it can wrap a level total",
          "[phase7][audit]") {
    // D25.7. Two orders of 2^63 at one price wrapped PriceLevel's cached sum, and
    // depth_at() then reported ZERO while 2^64 rested — with every invariant green,
    // because is_consistent() recomputes the sum with the same wrapping arithmetic.
    Engine             eng(kMin, kMax, 64);
    std::vector<Trade> out;

    REQUIRE(eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit, .price = 100,
                               .quantity = kMaxQuantity, .participant = 1}, out)
            != Engine::kRejected);
    CHECK(eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit, .price = 100,
                             .quantity = kMaxQuantity + 1, .participant = 1}, out)
          == Engine::kRejected);

    CHECK(eng.book().depth_at(100) == kMaxQuantity);   // exact, not wrapped
    REQUIRE(eng.check_invariants());
}

TEST_CASE("a marketable limit still trades when the pool is exhausted", "[phase7][audit]") {
    // D25.2. The F4 fix reserved a slot for EVERY limit order before matching, so at
    // capacity this order was rejected — an order that consumes resting liquidity and
    // frees slots, which is exactly what a full venue wants. The identical size sent as
    // a Market order traded, which is what made it obviously wrong rather than arguable.
    Engine             eng(kMin, kMax, 1);      // one slot, and the maker takes it
    std::vector<Trade> out;

    REQUIRE(eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit, .price = 100,
                               .quantity = 5, .participant = 1}, out) != Engine::kRejected);
    REQUIRE(eng.pool().in_use() == 1);          // pool is now full

    out.clear();
    const OrderId taker = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                             .price = 100, .quantity = 5, .participant = 2}, out);

    CHECK(taker != Engine::kRejected);          // was kRejected before D25.2
    REQUIRE(out.size() == 1);
    CHECK(out[0].quantity == 5);
    CHECK(eng.book().resting_count() == 0);
    CHECK(eng.pool().in_use() == 0);
    REQUIRE(eng.check_invariants());
}

TEST_CASE("a remainder rests on a slot its own fill freed", "[phase7][audit]") {
    // D25.2, second half. Letting the order through was not enough: cancelling its
    // remainder would have been the same mistake one step later. A surviving remainder
    // means every crossing maker was fully consumed, and retiring each returned its
    // slot — so a slot is free by construction and the remainder can rest.
    Engine     eng(kMin, kMax, 1);
    VectorSink sink;
    eng.set_sink(&sink);
    std::vector<Trade> out;

    REQUIRE(eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit, .price = 100,
                               .quantity = 3, .participant = 1}, out) != Engine::kRejected);
    REQUIRE(eng.pool().in_use() == 1);

    out.clear();
    const OrderId taker = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                             .price = 100, .quantity = 5, .participant = 2}, out);

    REQUIRE(taker != Engine::kRejected);
    REQUIRE(out.size() == 1);
    CHECK(out[0].quantity == 3);
    CHECK(eng.book().resting_count() == 1);          // the remainder rested
    CHECK(eng.book().depth_at(100) == 2);
    CHECK(eng.pool().in_use() == 1);                 // the maker's slot, reused
    REQUIRE(eng.check_invariants());

    // No cancel was emitted: nothing was refused.
    for (const Event& e : sink.events()) CHECK(!std::holds_alternative<OrderCancelled>(e));
    CHECK(props::check_conservation(sink.events(), eng.book()).ok);
}

TEST_CASE("a fully filled limit returns the slot it reserved", "[phase7][audit]") {
    // D25.4's guard on its ordinary path. The explicit release this replaced was
    // correct; what it could not do was survive an exception out of fill().
    Engine             eng(kMin, kMax, 8);
    std::vector<Trade> out;
    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit, .price = 100,
                       .quantity = 4, .participant = 1}, out);
    const std::size_t before = eng.pool().in_use();
    out.clear();
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit, .price = 100,
                       .quantity = 4, .participant = 2}, out);
    CHECK(eng.pool().in_use() == before - 1);        // maker's freed, taker's returned
    CHECK(eng.pool().free_list_is_consistent());
}

TEST_CASE("the book refuses id 0 rather than corrupting its index", "[phase7][audit]") {
    // D25.8. IdIndex uses id 0 as its EMPTY marker and Engine::kRejected is also 0.
    // add() validated the price unconditionally — citing D8's rule by name — and did
    // not apply that rule to the id one line later. Under NDEBUG the slot was written,
    // still read as empty, was never consumed, and count_ incremented anyway.
    OrderBook book(kMin, kMax, 8);
    Order     bad = make_order(0, Side::Buy, 101, 300, 1);
    CHECK_THROWS_AS(book.add(&bad), std::invalid_argument);
    CHECK(book.resting_count() == 0);
    CHECK(book.is_consistent());
}

TEST_CASE("the book refuses to overfill its id index rather than probing forever",
          "[phase7][audit]") {
    // D25.9. The probe loops were `for(;;)`, terminating only because an empty slot
    // always existed — guaranteed by an ASSERT on the load factor, which NDEBUG
    // deletes. With a full table a lookup of a missing id spun forever, and a hang is
    // the worst failure a trading process has: no crash, no core, no log.
    OrderBook book(kMin, kMax, 2);        // IdIndex sized for 2 -> 4 slots
    Order     a = make_order(1, Side::Buy, 101, 10, 1);
    Order     b = make_order(2, Side::Buy, 102, 10, 2);
    Order     c = make_order(3, Side::Buy, 103, 10, 3);

    book.add(&a);
    book.add(&b);
    CHECK_THROWS_AS(book.add(&c), std::length_error);

    CHECK(book.resting_count() == 2);     // refused before mutating anything
    CHECK(book.is_consistent());
    CHECK(book.find(999) == nullptr);     // and a miss TERMINATES
}

TEST_CASE("a const OrderBook yields a const Order", "[phase7][audit]") {
    // D25.6, enforced by the type system rather than by a runtime check. find() used to
    // return a mutable Order* from a const method, which made D23's const-only
    // Engine::book() decorative — mutating o->price through it and then cancelling
    // unlinked the order from a level that did not contain it.
    Engine eng(kMin, kMax, 8);
    static_assert(std::is_same_v<decltype(std::as_const(eng).book().find(OrderId{1})),
                                 const Order*>,
                  "const OrderBook::find must not hand out a mutable Order*");
    static_assert(std::is_same_v<decltype(std::declval<OrderBook&>().find(OrderId{1})),
                                 Order*>,
                  "non-const OrderBook::find stays mutable for the engine's own use");
    SUCCEED("checked at compile time");
}

TEST_CASE("conservation_holds_after_every_operation", "[phase7][fuzz]") {
    // Blueprint §9.2 asks for conservation after EVERY operation. It is O(log +
    // resting) per call, so the every-op version runs on a small stream and the
    // gate runs it at checkpoints and at the end. Both, rather than neither.
    Engine     eng(kMin, kMax, 1024);
    VectorSink sink;
    eng.set_sink(&sink);

    const auto         cmds = make_stream(31337u, 900);
    std::vector<Trade> scratch;

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        INFO("after operation " << i);
        scratch.clear();
        if (const auto* n = std::get_if<NewOrder>(&cmds[i])) eng.apply(*n, scratch);
        else                                                 eng.apply(*std::get_if<Cancel>(&cmds[i]));

        const auto c = props::check_conservation(sink.events(), eng.book());
        INFO(c.why);
        REQUIRE(c.ok);
    }
}

TEST_CASE("conservation_survives_pool_exhaustion", "[phase7][fuzz]") {
    // The path F4 was hiding on. A tiny pool guarantees rejections, and an
    // OrderRejected moves no quantity — so if the engine ever burns an id or
    // half-accepts an order, accepted stops matching filled+withdrawn+resting.
    Engine     eng(kMin, kMax, 16);
    VectorSink sink;
    eng.set_sink(&sink);

    feed(eng, make_stream(8080u, 600));

    bool saw_rejection = false;
    for (const Event& e : sink.events()) {
        if (const auto* r = std::get_if<OrderRejected>(&e)) {
            if (r->reason == RejectReason::PoolExhausted) { saw_rejection = true; break; }
        }
    }
    REQUIRE(saw_rejection);            // otherwise this test proves nothing

    const auto c = props::check_conservation(sink.events(), eng.book());
    INFO(c.why);
    REQUIRE(c.ok);
}

TEST_CASE("the_conservation_checker_catches_a_planted_violation", "[phase7][fuzz]") {
    // Same discipline as the property checker above: a checker that never fails
    // is an expensive way of computing `true`. One plant per check it makes.
    Engine     eng(kMin, kMax, 8192);
    VectorSink sink;
    eng.set_sink(&sink);
    feed(eng, make_stream(24601u, 700));

    const std::vector<Event> clean = sink.events();
    REQUIRE(props::check_conservation(clean, eng.book()).ok);

    auto index_of = [&clean](auto pred) -> std::size_t {
        for (std::size_t i = 0; i < clean.size(); ++i) if (pred(clean[i])) return i;
        return clean.size();
    };
    const std::size_t acc = index_of([](const Event& e) {
        const auto* a = std::get_if<OrderAccepted>(&e);
        return a != nullptr && a->type == OrderType::Limit;
    });
    const std::size_t trd = index_of([](const Event& e) {
        return std::holds_alternative<TradeExecuted>(e);
    });
    REQUIRE(acc < clean.size());
    REQUIRE(trd < clean.size());

    SECTION("a fill the book performed but the log omits") {
        auto bad = clean;
        bad.erase(bad.begin() + static_cast<std::ptrdiff_t>(trd));
        CHECK_FALSE(props::check_conservation(bad, eng.book()).ok);
    }
    SECTION("an order accepted for more than it really was") {
        auto bad = clean;
        std::get<OrderAccepted>(bad[acc]).quantity += 7;
        CHECK_FALSE(props::check_conservation(bad, eng.book()).ok);
    }
    SECTION("a cancel the book performed but the log omits") {
        auto bad = clean;
        const std::size_t cxl = index_of([](const Event& e) {
            return std::holds_alternative<OrderCancelled>(e);
        });
        REQUIRE(cxl < clean.size());        // not `if` — a skipped plant passes silently
        bad.erase(bad.begin() + static_cast<std::ptrdiff_t>(cxl));
        CHECK_FALSE(props::check_conservation(bad, eng.book()).ok);
    }
    SECTION("an order the log accepts and the book never held") {
        auto bad = clean;
        bad.push_back(OrderAccepted{.seq = 999'999, .id = 888'888, .side = Side::Buy,
                                    .type = OrderType::Limit, .price = 100, .quantity = 5});
        CHECK_FALSE(props::check_conservation(bad, eng.book()).ok);
    }
}

TEST_CASE("properties_hold_over_a_million_operations", "[.gate][phase7][fuzz]") {
    // The Blueprint's accept criterion. Properties are checked against the log
    // rather than the book, so this stays cheap enough to run at scale: no
    // per-operation O(range) invariant walk, just one pass over the events.
    constexpr int kOps = 1'000'000;

    const auto cmds = make_stream(20260907u, kOps);

    // The engine is constructed HERE rather than inside run_for_events because
    // conservation compares the log against the surviving book.
    Engine     eng(kMin, kMax, 65536);
    VectorSink sink;
    eng.set_sink(&sink);

    // Fed by hand rather than through feed(), so invariants can be checked periodically
    // DURING the run and not only at the end (D27).
    constexpr std::size_t kInvariantEvery = 10'000;
    std::vector<Trade>    scratch;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        scratch.clear();
        if (const auto* n = std::get_if<NewOrder>(&cmds[i])) eng.apply(*n, scratch);
        else                                                 eng.apply(*std::get_if<Cancel>(&cmds[i]));
        if ((i + 1) % kInvariantEvery == 0) {
            INFO("invariants broke by operation " << i);
            REQUIRE(eng.check_invariants());
        }
    }
    const std::vector<Event>& log = sink.events();

    REQUIRE(log.size() > static_cast<std::size_t>(kOps));   // it did real work

    // D27 — this gate called props::check and check_conservation and NOTHING ELSE, and
    // therefore passed an engine with time priority destroyed: make push_back push to
    // the FRONT and 1,676,622 events go by clean, because no log property can see the
    // order of a queue. check_invariants() is O(range + resting), so calling it after
    // every one of a million operations is not affordable — every 10,000 is, and that
    // is 100 chances to catch a FIFO break rather than none.
    REQUIRE(eng.check_invariants());

    const auto v = props::check(log, kMin, kMax);
    INFO(v.why << " at event " << v.at_event);
    REQUIRE(v.ok);

    // Blueprint §4.5, at full scale, against the book this log produced.
    const auto c = props::check_conservation(log, eng.book());
    INFO(c.why);
    REQUIRE(c.ok);
}

TEST_CASE("differential_holds_over_100k_operations_with_invariants", "[.gate][phase7][fuzz]") {
    // The expensive one: oracle diff AND all seven invariants after EVERY
    // operation. Smaller count because each step is O(range + resting).
    constexpr int kOps = 100'000;

    Engine           real(kMin, kMax, 65536);
    naive::NaiveBook ref(kMin, kMax);
    const auto       cmds = make_stream(20260908u, kOps);

    // Conservation folds the whole log, so checking it after EVERY operation
    // would make this O(ops^2). Every 5,000 instead — stated rather than
    // silently sampled. The per-operation version runs on a small stream below.
    VectorSink sink;
    real.set_sink(&sink);
    constexpr std::size_t kConservationEvery = 5'000;

    std::vector<Trade> got;
    std::vector<Trade> want;

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        INFO("diverged at operation " << i);
        got.clear();
        want.clear();

        if (const auto* n = std::get_if<NewOrder>(&cmds[i])) {
            REQUIRE(real.apply(*n, got) == ref.apply(*n, want));
            REQUIRE(got.size() == want.size());
            for (std::size_t j = 0; j < got.size(); ++j) {
                REQUIRE(got[j].maker_id == want[j].maker_id);
                REQUIRE(got[j].taker_id == want[j].taker_id);
                REQUIRE(got[j].price    == want[j].price);
                REQUIRE(got[j].quantity == want[j].quantity);
            }
        } else {
            const Cancel& c = *std::get_if<Cancel>(&cmds[i]);
            REQUIRE(real.apply(c) == ref.cancel(c.id));
        }

        REQUIRE(real.book().best_bid() == ref.best_bid());
        REQUIRE(real.book().best_ask() == ref.best_ask());
        REQUIRE(real.check_invariants());

        if ((i + 1) % kConservationEvery == 0) {
            const auto c = props::check_conservation(sink.events(), real.book());
            INFO(c.why);
            REQUIRE(c.ok);
            // D27/R7 — the depth sweep used to run ONCE, after the loop, so a quantity
            // leak at a non-best level was never localised to an operation. The two
            // small differentials compare it every operation; at 100k that is too slow,
            // so it rides along with the conservation checkpoint.
            for (Price p = kMin; p <= kMax; ++p) {
                REQUIRE(real.book().depth_at(p) == ref.depth_at(p));
            }
        }
    }

    {
        const auto c = props::check_conservation(sink.events(), real.book());
        INFO(c.why);
        REQUIRE(c.ok);
    }

    for (Price p = kMin; p <= kMax; ++p) {
        REQUIRE(real.book().depth_at(p) == ref.depth_at(p));
    }
}

TEST_CASE("the_property_checker_catches_a_planted_violation", "[phase7][fuzz]") {
    // A checker that never fails proves nothing. Corrupt a log four ways and
    // confirm each one is caught — otherwise the million-operation run above is
    // a very expensive way of computing `true`.
    const auto cmds = make_stream(555u, 400);
    const auto clean = run_for_events(cmds);
    REQUIRE(props::check(clean, kMin, kMax).ok);

    auto first_trade = [](const std::vector<Event>& log) -> std::size_t {
        for (std::size_t i = 0; i < log.size(); ++i) {
            if (std::holds_alternative<TradeExecuted>(log[i])) return i;
        }
        return log.size();
    };
    const std::size_t t = first_trade(clean);
    REQUIRE(t < clean.size());

    SECTION("a trade not at the maker's price") {
        auto bad = clean;
        std::get<TradeExecuted>(bad[t]).price += 1;
        CHECK_FALSE(props::check(bad, kMin, kMax).ok);
    }
    SECTION("a trade for more than the order held") {
        auto bad = clean;
        std::get<TradeExecuted>(bad[t]).quantity += 1'000'000;
        CHECK_FALSE(props::check(bad, kMin, kMax).ok);
    }
    SECTION("a trade with an unknown maker") {
        auto bad = clean;
        std::get<TradeExecuted>(bad[t]).maker_id = 999'999;
        CHECK_FALSE(props::check(bad, kMin, kMax).ok);
    }
    SECTION("a cancelled order trading afterwards") {
        auto bad = clean;
        const OrderId maker = std::get<TradeExecuted>(bad[t]).maker_id;
        bad.insert(bad.begin() + static_cast<long>(t),
                   OrderCancelled{.seq = 0, .id = maker, .reason = CancelReason::UserRequested});
        CHECK_FALSE(props::check(bad, kMin, kMax).ok);
    }
}

TEST_CASE("the_shrinker_reduces_a_failing_stream", "[.gate][phase7][fuzz]") {
    // Shrinking is the one thing a property library gives that a seeded loop
    // does not, so it needs its own test. Predicate: "the log contains a trade
    // at price 100" — arbitrary, but it depends on a specific few commands, so
    // a correct shrinker must find them and drop everything else.
    const auto cmds = make_stream(31415u, 4000);

    auto has_trade_at_100 = [](const std::vector<scenario::Command>& c) {
        for (const Event& e : run_for_events(c)) {
            if (const auto* t = std::get_if<TradeExecuted>(&e)) {
                if (t->price == 100) return true;
            }
        }
        return false;
    };

    REQUIRE(has_trade_at_100(cmds));
    const auto small = me::shrink::minimise(cmds, has_trade_at_100);

    INFO("shrank " << cmds.size() << " commands to " << small.size());
    CHECK(has_trade_at_100(small));            // still reproduces — the real property

    // D27/1.22. This asserted `< cmds.size() / 10`, i.e. fewer than 400, and the
    // shrinker actually returns TWO. A "keep the first K commands" stub would also
    // have passed at 400, so the assertion accepted almost any behaviour. A trade
    // needs a resting order and an order that crosses it, so 2 is the floor.
    CHECK(small.size() <= 4);
}
