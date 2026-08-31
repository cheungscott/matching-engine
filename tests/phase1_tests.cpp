// tests/phase1_tests.cpp — the Phase 1 acceptance suite.
//
// These are RED on purpose. Every one of them fails right now because the
// bodies in include/me/*.hpp are stubs. Making them green IS Phase 1.
//
// Blueprint §11 Phase 1 accept criteria:
//   ObjectPool<Order> · PriceLevel intrusive list · OrderBook add + BBO cursors
//   · apply(NewOrder) for rest-on-empty and exact full-fill at one price
//   · unit tests green · ASan clean
//
// Build (Catch2 arrives via CMake FetchContent; run from WSL/Linux):
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
//   cmake --build build && ctest --test-dir build --output-on-failure
//
// The Debug config is the one that counts: it carries ASan/UBSan. A green suite
// under a MinGW build verifies logic only; only the sanitized Linux build
// verifies that your pointer surgery did not corrupt memory, and Phase 1 is
// exactly where the first dangling Order* shows up.
//

#include "me/engine.hpp"
#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/price_level.hpp"
#include "me/types.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace me;

namespace {

// Build a resting order without going through the pool, for the container tests.
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
    if (book.best_bid()) CHECK(*book.best_bid() == Price{101});
    CHECK(!book.best_ask().has_value());
}

TEST_CASE("book_best_bid_is_the_highest", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    Order lo = make_order(1, Side::Buy, 100, 500, 1);
    Order hi = make_order(2, Side::Buy, 101, 300, 2);
    book.add(&lo);
    book.add(&hi);
    if (book.best_bid()) CHECK(*book.best_bid() == Price{101});

    // Adding a WORSE bid must not move the cursor.
    Order worse = make_order(3, Side::Buy, 99, 100, 3);
    book.add(&worse);
    if (book.best_bid()) CHECK(*book.best_bid() == Price{101});
}

TEST_CASE("book_best_ask_is_the_lowest", "[phase1][book]") {
    OrderBook book(kMin, kMax);
    Order hi = make_order(1, Side::Sell, 103, 200, 1);
    Order lo = make_order(2, Side::Sell, 102, 250, 2);
    book.add(&hi);
    book.add(&lo);
    if (book.best_ask()) CHECK(*book.best_ask() == Price{102});

    Order worse = make_order(3, Side::Sell, 104, 100, 3);
    book.add(&worse);
    if (book.best_ask()) CHECK(*book.best_ask() == Price{102});
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
    if (book.best_bid() && book.best_ask()) CHECK(*book.best_bid() < *book.best_ask());
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
    if (eng.book().best_bid()) CHECK(*eng.book().best_bid() == Price{101});
}

TEST_CASE("engine_does_not_match_when_it_does_not_cross", "[phase1][engine]") {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 2}, trades);

    CHECK(trades.size() == std::size_t{0});
    if (eng.book().best_bid()) CHECK(*eng.book().best_bid() == Price{101});
    if (eng.book().best_ask()) CHECK(*eng.book().best_ask() == Price{102});
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
