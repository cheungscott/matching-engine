// me/engine.hpp — match first, rest second.
//
// The only component that decides anything. The book stores, the level orders,
// the pool recycles; policy lives here.
//
// PHASE 1 SCOPE. Partial fills are Phase 2, multi-level walks and market orders
// Phase 3, cancel Phase 4. The boundary is enforced by an assert, not left to
// silently do the wrong thing — see D11.
//
// Rationale in SYSTEM-DESIGN.md D11. .
#pragma once

#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/types.hpp"

#include <cassert>
#include <cstddef>
#include <vector>

namespace me {

// A command arriving from outside. NOT an Order: it has no id, no queue
// position and no remaining — the engine assigns those, which is what stops a
// client choosing its own place in the FIFO queue.
struct NewOrder {
    Side          side{};
    OrderType     type{};
    Price         price{};
    Quantity      quantity{};
    ParticipantId participant{};
};

class Engine {
public:
    // 0 is never a valid order id, so it doubles as "rejected". Phase 3 replaces
    // this with std::expected<OrderId, RejectReason>, which g++ 11 lacks.
    static constexpr OrderId kRejected = 0;

    Engine(Price min_price, Price max_price, std::size_t pool_capacity)
        : book_(min_price, max_price), pool_(pool_capacity) {}

    // Match what crosses, rest what remains. Trades are appended to `out`.
    // Returns the engine-assigned id, or kRejected.
    OrderId apply(const NewOrder& cmd, std::vector<Trade>& out) {
        if (!validate(cmd)) {
            return kRejected;
        }

        const SeqNum  seq = next_seq_++;
        const OrderId id  = next_id_++;

        if (book_.crosses(cmd.side, cmd.price)) {
            fill(cmd, id, seq, out);
            return id;                      // fully filled; never rests
        }

        Order* resting = pool_.acquire();
        if (resting == nullptr) {
            return kRejected;               // pool exhausted: an honest bounded failure
        }

        resting->id          = id;
        resting->side        = cmd.side;
        resting->type        = cmd.type;
        resting->price       = cmd.price;
        resting->quantity    = cmd.quantity;
        resting->remaining   = cmd.quantity;
        resting->entry_seq   = seq;
        resting->participant = cmd.participant;

        book_.add(resting);
        return id;
    }

    [[nodiscard]] OrderBook&       book()       noexcept { return book_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const ObjectPool<Order>& pool() const noexcept { return pool_; }

private:
    [[nodiscard]] bool validate(const NewOrder& cmd) const noexcept {
        if (cmd.quantity == 0)             return false;
        if (cmd.type != OrderType::Limit)  return false;   // Market is Phase 3
        if (!book_.in_range(cmd.price))    return false;   // the bounded-array reject
        return true;
    }

    // Phase 1: exactly one resting order, at one price, for the whole quantity.
    void fill(const NewOrder& cmd, OrderId taker_id, SeqNum seq, std::vector<Trade>& out) {
        const Side  opposite = (cmd.side == Side::Buy) ? Side::Sell : Side::Buy;
        PriceLevel* level    = book_.best_level(opposite);
        assert(level != nullptr && "crosses() said there was liquidity");

        Order* maker = level->front();
        assert(maker != nullptr && "occupied cursor pointing at an empty level");
        assert(maker->remaining == cmd.quantity &&
               "Phase 1 handles exact full fills only; partial fills are Phase 2");

        // At the MAKER's price. The resting order set the terms; the aggressor
        // accepted them, so the taker may receive price improvement.
        out.push_back(Trade{
            .seq      = seq,
            .maker_id = maker->id,
            .taker_id = taker_id,
            .price    = maker->price,
            .quantity = maker->remaining,
        });

        // Order matters: unlink subtracts maker->remaining from the level's
        // cached total, so remaining must still be intact here (D9).
        const Price price = maker->price;
        level->unlink(maker);
        if (level->empty()) {
            book_.on_level_emptied(opposite, price);
        }
        pool_.release(maker);
    }

    OrderBook         book_;
    ObjectPool<Order> pool_;
    SeqNum            next_seq_ = 1;    // arrival order IS time priority
    OrderId           next_id_  = 1;    // 0 reserved for kRejected
};

} // namespace me
