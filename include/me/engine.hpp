// me/engine.hpp — match first, rest second.
//
// The only component that decides anything. The book stores, the level orders,
// the pool recycles; policy lives here.
//
// CURRENT SCOPE: no partial fills, multi-level walks, market orders or cancel yet.
// The boundary is enforced by an assert, not left to
// silently do the wrong thing.
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
    // 0 is never a valid order id, so it doubles as "rejected". A later change replaces
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

        const SeqNum  arrival = next_seq_++;
        const OrderId id      = next_id_++;

        Quantity remaining = cmd.quantity;
        if (book_.crosses(cmd.side, cmd.price)) {
            fill(cmd, id, remaining, out);
        }
        if (remaining == 0) {
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
        resting->remaining   = remaining;   // what survived the fill loop
        resting->entry_seq   = arrival;
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
        if (cmd.type != OrderType::Limit)  return false;   // Market orders are not supported yet
        if (!book_.in_range(cmd.price))    return false;   // the bounded-array reject
        return true;
    }

    // Consume resting orders oldest-first while the incoming order still has
    // quantity and still crosses. Decrements `remaining` in place.
    //
    // Within ONE price level. Walking to the next level comes later,
    // and the assert below is that boundary.
    void fill(const NewOrder& cmd, OrderId taker_id, Quantity& remaining,
              std::vector<Trade>& out) {
        const Side  opposite   = (cmd.side == Side::Buy) ? Side::Sell : Side::Buy;
        const Price first_best = book_.best_level(opposite)->price();

        while (remaining > 0 && book_.crosses(cmd.side, cmd.price)) {
            PriceLevel* level = book_.best_level(opposite);
            assert(level != nullptr && "crosses() said there was liquidity");
            assert(level->price() == first_best &&
                   "fills stay within one level; walking levels comes later");

            Order* maker = level->front();
            assert(maker != nullptr && "cursor points at an empty level");

            const Quantity traded = (remaining < maker->remaining) ? remaining : maker->remaining;

            // At the MAKER's price. The resting order set the terms; the
            // aggressor accepted them, so the taker may take price improvement.
            out.push_back(Trade{
                .seq      = next_seq_++,
                .maker_id = maker->id,
                .taker_id = taker_id,
                .price    = maker->price,
                .quantity = traded,
            });
            remaining -= traded;

            if (traded == maker->remaining) {
                // Fully consumed. Unlink BEFORE releasing, and read the price
                // before the slot is poisoned.
                const Price price = maker->price;
                level->unlink(maker);
                if (level->empty()) {
                    book_.on_level_emptied(opposite, price);
                }
                pool_.release(maker);
            } else {
                // Partially consumed: it keeps its queue position.
                level->reduce_front(traded);
            }
        }
    }

    OrderBook         book_;
    ObjectPool<Order> pool_;
    SeqNum            next_seq_ = 1;    // arrival order IS time priority
    OrderId           next_id_  = 1;    // 0 reserved for kRejected
};

} // namespace me
