// me/engine.hpp — match first, rest second. Phase 1.
//
// ===========================================================================
//  THE BODIES ARE YOURS.
// ===========================================================================
//
// PHASE 1 SCOPE ONLY. Deliberately excluded, do not build them yet:
//   - partial fills                      Phase 2
//   - walking multiple price levels      Phase 3
//   - market orders                      Phase 3
//   - cancel                             Phase 4
//   - amend                              Phase 5
// Phase 1 is: rest on an empty book, and an EXACT full fill at ONE price.
//
// THE TWO RULES THIS PHASE EXISTS TO ENCODE:
//
//   1. Trades print at the MAKER's price. The resting order set the terms; the
//      aggressor accepted them. A buyer willing to pay 103 who meets an ask at
//      102 trades at 102 and receives price improvement. Get this backwards and
//      every downstream P&L number is wrong.
//
//   2. "At or better" INCLUDES EQUAL. An order priced exactly at the opposite
//      best must TRADE, not rest. The strict-inequality version leaves a locked
//      book, which is a missed trade, which is a correctness bug — not a market
//      condition. This is the single most common student matching bug and it is
//      one character wide.
//
// PROVISIONAL API, and knowingly so: trades go into a caller-owned vector.
// Allocating per call would violate everything in Module 2, but Phase 1 is about
// correctness and the vector is reused across calls by the caller. Phase 6
// replaces this with a proper EventSink. Logged so it does not become permanent
// by accident.
#pragma once

#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/types.hpp"

#include <vector>

namespace me {

// A command arriving at the engine. Not the same thing as a resting Order:
// this has no id and no queue position yet — the engine assigns those.
struct NewOrder {
    Side          side{};
    OrderType     type{};
    Price         price{};
    Quantity      quantity{};
    ParticipantId participant{};
};

class Engine {
public:
    Engine(Price min_price, Price max_price, std::size_t pool_capacity)
        : book_(min_price, max_price), pool_(pool_capacity) {}

    // Match what crosses, rest what remains. Appends any trades to `out`.
    // Returns the engine-assigned id of the incoming order.
    OrderId apply(const NewOrder& /*cmd*/, std::vector<Trade>& /*out*/) {
        // TODO(you). The shape, in prose (Blueprint §4.2):
        //   while the incoming order still has quantity
        //     and the best opposite level CROSSES it:
        //       fill against the FRONT of that level
        //       print the trade at the RESTING order's price
        //       a fully-filled resting order is unlinked and returned to the pool
        //   whatever remains rests (Phase 1: it either fully filled, or it rests
        //   untouched — no partial fills yet)
        return 0;
    }

    [[nodiscard]] OrderBook&       book()       noexcept { return book_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

private:
    OrderBook        book_;
    ObjectPool<Order> pool_;
    SeqNum           next_seq_ = 1;   // arrival order == time priority
    OrderId          next_id_  = 1;
};

} // namespace me
