// me/engine.hpp — match first, rest second.
//
// The only component that decides anything. The book stores, the level orders,
// the pool recycles; policy lives here.
//
// Handles NewOrder (limit and market) and Cancel. Amend is CUT from v0.1.
//
// Rationale in SYSTEM-DESIGN.md D11-D14. .
#pragma once

#include "me/events.hpp"
#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/types.hpp"

#include <cassert>
#include <cstddef>
#include <optional>
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

// Cancel a resting order by the id the engine assigned at accept time. That id
// is ALL a real cancel message carries, which is why the book needs an index
// (Blueprint §3.4).
struct Cancel {
    OrderId id{};
};

class Engine {
public:
    // 0 is never a valid order id, so it doubles as "rejected". Phase 3 replaces
    // this with std::expected<OrderId, RejectReason>, which g++ 11 lacks.
    static constexpr OrderId kRejected = 0;

    // The index is sized from the pool, exactly: an order that cannot be pooled
    // cannot rest, so `pool_capacity` is a hard bound on live index entries and
    // the index never needs to grow (D19).
    Engine(Price min_price, Price max_price, std::size_t pool_capacity)
        : book_(min_price, max_price, pool_capacity), pool_(pool_capacity) {}

    // Match what crosses, rest what remains. Trades are appended to `out`.
    // Returns the engine-assigned id, or kRejected.
    OrderId apply(const NewOrder& cmd, std::vector<Trade>& out) {
        if (const auto why = validate(cmd); why.has_value()) {
            emit(OrderRejected{.seq = next_seq_++, .reason = *why});
            return kRejected;
        }

        // D19: a limit order reserves its slot BEFORE it is accepted.
        //
        // Previously the accept was emitted first and the pool consulted after
        // matching, so exhaustion produced Accepted-then-Rejected — and the
        // reject carries no id, leaving a replayer unable to undo the accept.
        // A log that cannot be folded back into the book breaks the one claim
        // the event-sourced design makes. Reserving first means an order is
        // only ever accepted if the engine can honour it.
        //
        // Markets never rest, so they need no slot. The cost for limits is one
        // acquire/release pair on the fully-filled path, and both are pointer
        // bumps.
        Order* slot = nullptr;
        if (cmd.type == OrderType::Limit) {
            slot = pool_.acquire();
            if (slot == nullptr) {
                emit(OrderRejected{.seq = next_seq_++, .reason = RejectReason::PoolExhausted});
                return kRejected;           // no id burned: it never existed
            }
        }

        const SeqNum  arrival = next_seq_++;
        const OrderId id      = next_id_++;
        emit(OrderAccepted{.seq = arrival, .id = id, .side = cmd.side, .type = cmd.type,
                           .price = canonical_price(cmd), .quantity = cmd.quantity});

        Quantity remaining = cmd.quantity;
        fill(cmd, id, remaining, out);

        if (remaining == 0) {
            if (slot != nullptr) {
                const bool freed = pool_.release(slot);   // reserved, not needed
                assert(freed && "reserved slot could not be returned");
                (void)freed;
            }
            return id;                      // fully filled; never rests
        }

        // A market order NEVER rests: it wanted liquidity now, not a queue
        // position. Whatever it could not fill is cancelled.
        if (cmd.type == OrderType::Market) {
            emit(OrderCancelled{.seq = next_seq_++, .id = id,
                                .reason = CancelReason::NoLiquidity});
            return id;
        }

        Order* resting = slot;
        assert(resting != nullptr && "a limit order always reserves a slot");

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

    // Cancel a resting order. false means no such order is resting — which is
    // ROUTINE, not an error: a fill and a cancel legitimately race, and the
    // fill can win (Blueprint §5.4).
    bool apply(const Cancel& cmd) noexcept {
        Order* o = book_.find(cmd.id);
        if (o == nullptr) {
            emit(OrderRejected{.seq = next_seq_++, .reason = RejectReason::UnknownOrder});
            return false;
        }
        retire(o);                      // the one removal path (D14)
        emit(OrderCancelled{.seq = next_seq_++, .id = cmd.id,
                            .reason = CancelReason::UserRequested});
        return true;
    }

    // Attach a destination for the sequenced event stream. Null means discard,
    // which is what every pre-Phase-6 test does.
    void set_sink(EventSink* sink) noexcept { sink_ = sink; }

    [[nodiscard]] OrderBook&       book()       noexcept { return book_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const ObjectPool<Order>& pool() const noexcept { return pool_; }

    // Blueprint §3.6, all seven. O(range + resting). Tests only.
    [[nodiscard]] bool check_invariants() const noexcept {
        if (!book_.is_consistent())            return false;   // 1-6
        if (!pool_.free_list_is_consistent())  return false;   // 7, structure
        // 7, the accounting half: every resting order holds exactly one slot,
        // and nothing else holds one. A leaked slot shows up here and nowhere
        // else, because the book cannot see a slot it has lost track of.
        return pool_.in_use() == book_.resting_count();
    }

private:
    // THE removal path. book_.remove() drops the order from its level, the
    // index and the cursor in one step; release() returns the slot. Every
    // removal in the engine goes through here — fill-to-zero, cancel, and
    // amend when it arrives — so invariant 7 has exactly one place to break.
    void retire(Order* o) noexcept {
        const bool removed = book_.remove(o);
        assert(removed && "retire(): book refused the order");
        (void)removed;
        // D8 says the pool REPORTS and the caller DECIDES, and this is the call
        // site that decides. Dropping the result made the whole return-bool
        // design terminate in a shrug (F6).
        const bool freed = pool_.release(o);
        assert(freed && "retire(): pool refused the slot — double release or foreign pointer");
        (void)freed;
    }

    // A market order's price field is meaningless, so it is normalised to 0
    // before it reaches the event log (D19). Without this, two behaviourally
    // identical market orders carrying different junk produce different
    // byte-for-byte logs — a canonicality hole in the exact artefact the replay
    // test diffs.
    [[nodiscard]] static Price canonical_price(const NewOrder& cmd) noexcept {
        return (cmd.type == OrderType::Market) ? Price{0} : cmd.price;
    }

    // nullopt means accepted. Returning the REASON rather than a bool is what
    // lets the reject event say something useful (Blueprint §5.1).
    [[nodiscard]] std::optional<RejectReason> validate(const NewOrder& cmd) const noexcept {
        if (cmd.quantity == 0) return RejectReason::InvalidQuantity;
        // A market order carries no meaningful price, so the tick-window check
        // applies only to limits.
        if (cmd.type == OrderType::Limit && !book_.in_range(cmd.price)) {
            return RejectReason::PriceOutOfRange;
        }
        return std::nullopt;
    }

    void emit(const Event& e) noexcept {
        if (sink_ != nullptr) sink_->publish(e);
    }

    // Can this order trade against what is resting right now?
    // A limit crosses on price; a market takes whatever exists at any price.
    [[nodiscard]] bool can_match(const NewOrder& cmd) const noexcept {
        const bool opposite_has_liquidity =
            (cmd.side == Side::Buy) ? book_.best_ask().has_value()
                                    : book_.best_bid().has_value();
        if (cmd.type == OrderType::Market) return opposite_has_liquidity;
        return book_.crosses(cmd.side, cmd.price);
    }

    // Consume resting orders oldest-first, walking outward through price levels,
    // while the incoming order still has quantity and can still trade.
    // Decrements `remaining` in place.
    void fill(const NewOrder& cmd, OrderId taker_id, Quantity& remaining,
              std::vector<Trade>& out) {
        const Side opposite = (cmd.side == Side::Buy) ? Side::Sell : Side::Buy;

        while (remaining > 0 && can_match(cmd)) {
            PriceLevel* level = book_.best_level(opposite);
            assert(level != nullptr && "can_match() said there was liquidity");

            Order* maker = level->front();
            assert(maker != nullptr && "cursor points at an empty level");

            const Quantity traded = (remaining < maker->remaining) ? remaining : maker->remaining;

            // At the MAKER's price. The resting order set the terms; the
            // aggressor accepted them, so the taker may take price improvement.
            const SeqNum tseq = next_seq_++;
            out.push_back(Trade{
                .seq      = tseq,
                .maker_id = maker->id,
                .taker_id = taker_id,
                .price    = maker->price,
                .quantity = traded,
            });
            emit(TradeExecuted{.seq = tseq, .maker_id = maker->id, .taker_id = taker_id,
                               .price = maker->price, .quantity = traded});
            remaining -= traded;

            if (traded == maker->remaining) {
                retire(maker);              // the one removal path (D14)
            } else {
                // Partially consumed: it keeps its queue position (D12).
                level->reduce_front(traded);
            }
        }
    }

    OrderBook         book_;
    ObjectPool<Order> pool_;
    SeqNum            next_seq_ = 1;    // arrival order IS time priority
    OrderId           next_id_  = 1;    // 0 reserved for kRejected
    EventSink*        sink_     = nullptr;
};

} // namespace me
