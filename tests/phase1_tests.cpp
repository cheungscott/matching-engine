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
// Build (works with g++ alone, no cmake, no network):
//   g++ -std=c++23 -Wall -Wextra -Wconversion -Iinclude tests/phase1_tests.cpp -o p1 && ./p1
//
// Build WITH the memory checker (Linux/WSL only — MinGW has no libasan):
//   g++ -std=c++23 -O0 -g -fsanitize=address,undefined -Iinclude tests/phase1_tests.cpp -o p1 && ./p1
//
// The second command is the one that counts. The first verifies logic; only the
// second verifies that your pointer surgery did not corrupt memory, and Phase 1
// is exactly where the first dangling Order* shows up.

#include "me/engine.hpp"
#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/price_level.hpp"
#include "me/types.hpp"

#include "check.hpp"

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

TEST(pool_reports_capacity) {
    ObjectPool<Order> pool(8);
    CHECK_EQ(pool.capacity(), std::size_t{8});
    CHECK_EQ(pool.in_use(), std::size_t{0});
}

TEST(pool_acquire_returns_distinct_live_slots) {
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    CHECK_EQ(pool.in_use(), std::size_t{2});
}

TEST(pool_release_recycles_rather_than_leaking) {
    ObjectPool<Order> pool(2);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    CHECK_EQ(pool.in_use(), std::size_t{2});

    pool.release(a);
    CHECK_EQ(pool.in_use(), std::size_t{1});

    // A pool of 2 that has released one MUST be able to hand one out again.
    // If this returns nullptr you are leaking slots, not recycling them.
    Order* c = pool.acquire();
    CHECK(c != nullptr);
    CHECK_EQ(pool.in_use(), std::size_t{2});

    pool.release(b);
    pool.release(c);
    CHECK_EQ(pool.in_use(), std::size_t{0});
}

TEST(pool_slots_are_writable_after_acquire) {
    // If you poison on release, you MUST unpoison on acquire. Under ASan this
    // test is what catches a missing unpoison — without it, the write below
    // reports use-after-poison on a slot you legitimately own.
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire();
    CHECK(a != nullptr);
    if (a != nullptr) {
        *a = make_order(1, Side::Buy, 100, 50, 1);
        CHECK_EQ(a->price, Price{100});
    }
    pool.release(a);

    Order* b = pool.acquire();       // very likely the same slot, recycled
    CHECK(b != nullptr);
    if (b != nullptr) {
        *b = make_order(2, Side::Sell, 105, 25, 2);
        CHECK_EQ(b->price, Price{105});
    }
}

// ===========================================================================
//  PriceLevel — FIFO by arrival, O(1) unlink, cached total
// ===========================================================================

TEST(level_starts_empty) {
    PriceLevel lvl(102);
    CHECK(lvl.empty());
    CHECK(lvl.front() == nullptr);
    CHECK_EQ(lvl.total_quantity(), Quantity{0});
    CHECK_EQ(lvl.price(), Price{102});
}

TEST(level_push_back_one) {
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    lvl.push_back(&a);

    CHECK(!lvl.empty());
    CHECK(lvl.front() == &a);
    CHECK_EQ(lvl.total_quantity(), Quantity{100});
}

TEST(level_is_fifo_oldest_at_front) {
    // A3 arrives before A4, so A3 fills first. This is time priority, and it is
    // structural: fills come off the head, so being at the head IS being first.
    PriceLevel lvl(102);
    Order a3 = make_order(3, Side::Sell, 102, 100, 3);
    Order a4 = make_order(4, Side::Sell, 102, 150, 4);
    lvl.push_back(&a3);
    lvl.push_back(&a4);

    CHECK(lvl.front() == &a3);
    CHECK_EQ(lvl.total_quantity(), Quantity{250});
}

TEST(level_unlink_head_promotes_the_next) {
    PriceLevel lvl(102);
    Order a3 = make_order(3, Side::Sell, 102, 100, 3);
    Order a4 = make_order(4, Side::Sell, 102, 150, 4);
    lvl.push_back(&a3);
    lvl.push_back(&a4);

    lvl.unlink(&a3);
    CHECK(lvl.front() == &a4);
    CHECK_EQ(lvl.total_quantity(), Quantity{150});
    CHECK(!lvl.empty());
}

TEST(level_unlink_tail_keeps_head) {
    PriceLevel lvl(102);
    Order a3 = make_order(3, Side::Sell, 102, 100, 3);
    Order a4 = make_order(4, Side::Sell, 102, 150, 4);
    lvl.push_back(&a3);
    lvl.push_back(&a4);

    lvl.unlink(&a4);
    CHECK(lvl.front() == &a3);
    CHECK_EQ(lvl.total_quantity(), Quantity{100});
}

TEST(level_unlink_middle_keeps_list_intact) {
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
    CHECK_EQ(lvl.total_quantity(), Quantity{40});
    CHECK(lvl.front() == &a);

    // Walk what is left: a then c, and nothing else.
    lvl.unlink(&a);
    CHECK(lvl.front() == &c);
    lvl.unlink(&c);
    CHECK(lvl.empty());
    CHECK_EQ(lvl.total_quantity(), Quantity{0});
}

TEST(level_unlink_only_element_empties_it) {
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    lvl.push_back(&a);
    lvl.unlink(&a);

    CHECK(lvl.empty());
    CHECK(lvl.front() == nullptr);
    CHECK_EQ(lvl.total_quantity(), Quantity{0});
}

TEST(level_is_reusable_after_being_emptied) {
    // An emptied level is not a dead level — the price will be quoted again.
    PriceLevel lvl(102);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    lvl.push_back(&a);
    lvl.unlink(&a);

    Order b = make_order(2, Side::Sell, 102, 70, 2);
    lvl.push_back(&b);
    CHECK(lvl.front() == &b);
    CHECK_EQ(lvl.total_quantity(), Quantity{70});
}

// ===========================================================================
//  OrderBook — add and the BBO cursors
// ===========================================================================

TEST(book_starts_with_no_bbo) {
    OrderBook book(kMin, kMax);
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
}

TEST(book_add_buy_sets_best_bid_only) {
    OrderBook book(kMin, kMax);
    Order b = make_order(1, Side::Buy, 101, 300, 1);
    book.add(&b);

    CHECK(book.best_bid().has_value());
    if (book.best_bid()) CHECK_EQ(*book.best_bid(), Price{101});
    CHECK(!book.best_ask().has_value());
}

TEST(book_best_bid_is_the_highest) {
    OrderBook book(kMin, kMax);
    Order lo = make_order(1, Side::Buy, 100, 500, 1);
    Order hi = make_order(2, Side::Buy, 101, 300, 2);
    book.add(&lo);
    book.add(&hi);
    if (book.best_bid()) CHECK_EQ(*book.best_bid(), Price{101});

    // Adding a WORSE bid must not move the cursor.
    Order worse = make_order(3, Side::Buy, 99, 100, 3);
    book.add(&worse);
    if (book.best_bid()) CHECK_EQ(*book.best_bid(), Price{101});
}

TEST(book_best_ask_is_the_lowest) {
    OrderBook book(kMin, kMax);
    Order hi = make_order(1, Side::Sell, 103, 200, 1);
    Order lo = make_order(2, Side::Sell, 102, 250, 2);
    book.add(&hi);
    book.add(&lo);
    if (book.best_ask()) CHECK_EQ(*book.best_ask(), Price{102});

    Order worse = make_order(3, Side::Sell, 104, 100, 3);
    book.add(&worse);
    if (book.best_ask()) CHECK_EQ(*book.best_ask(), Price{102});
}

TEST(book_best_level_exposes_the_right_queue) {
    OrderBook book(kMin, kMax);
    Order a = make_order(1, Side::Sell, 102, 100, 1);
    book.add(&a);

    PriceLevel* lvl = book.best_level(Side::Sell);
    CHECK(lvl != nullptr);
    if (lvl != nullptr) {
        CHECK_EQ(lvl->price(), Price{102});
        CHECK(lvl->front() == &a);
    }
    CHECK(book.best_level(Side::Buy) == nullptr);
}

TEST(book_stays_uncrossed_invariant_1) {
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

TEST(engine_rests_an_order_on_an_empty_book) {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 300, .participant = 1}, trades);

    CHECK_EQ(trades.size(), std::size_t{0});
    CHECK(eng.book().best_bid().has_value());
    if (eng.book().best_bid()) CHECK_EQ(*eng.book().best_bid(), Price{101});
}

TEST(engine_does_not_match_when_it_does_not_cross) {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 2}, trades);

    CHECK_EQ(trades.size(), std::size_t{0});
    if (eng.book().best_bid()) CHECK_EQ(*eng.book().best_bid(), Price{101});
    if (eng.book().best_ask()) CHECK_EQ(*eng.book().best_ask(), Price{102});
}

TEST(engine_exact_full_fill_at_one_price) {
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 2}, trades);

    CHECK_EQ(trades.size(), std::size_t{1});
    if (trades.size() == 1) {
        CHECK_EQ(trades[0].price, Price{102});
        CHECK_EQ(trades[0].quantity, Quantity{100});
    }
    // Both sides fully consumed: the book is empty again.
    CHECK(!eng.book().best_bid().has_value());
    CHECK(!eng.book().best_ask().has_value());
}

TEST(engine_prints_the_trade_at_the_makers_price) {
    // The taker is willing to pay 103. The resting ask is at 102. The trade
    // MUST print at 102 — the maker set the terms first. The taker receives
    // price improvement. Backwards here and every P&L number downstream is wrong.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 102, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 103, .quantity = 100, .participant = 2}, trades);

    CHECK_EQ(trades.size(), std::size_t{1});
    if (trades.size() == 1) CHECK_EQ(trades[0].price, Price{102});
}

TEST(engine_treats_equal_prices_as_crossing) {
    // THE ONE-CHARACTER BUG. An order priced exactly AT the opposite best must
    // trade, not rest. `<` instead of `<=` leaves an ask at 101 sitting beside
    // a bid at 101 — a locked book, which is a missed trade.
    Engine eng(kMin, kMax, 64);
    std::vector<Trade> trades;

    eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 1}, trades);
    eng.apply(NewOrder{.side = Side::Sell, .type = OrderType::Limit,
                       .price = 101, .quantity = 100, .participant = 2}, trades);

    CHECK_EQ(trades.size(), std::size_t{1});
    if (trades.size() == 1) CHECK_EQ(trades[0].price, Price{101});
    CHECK(!eng.book().best_bid().has_value());
    CHECK(!eng.book().best_ask().has_value());
}

TEST(engine_attributes_maker_and_taker) {
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

    CHECK_EQ(trades.size(), std::size_t{1});
    if (trades.size() == 1) {
        CHECK_EQ(trades[0].maker_id, maker);
        CHECK_EQ(trades[0].taker_id, taker);
    }
}
