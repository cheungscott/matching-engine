// me/engine.hpp — match first, rest second.
//
// The only component that decides anything. The book stores, the level orders,
// the pool recycles; policy lives here.
//
// Handles NewOrder (limit and market) and Cancel. Amend is CUT from v0.1.
//
// Rationale in SYSTEM-DESIGN.md D11-D14.
#pragma once

#include "me/events.hpp"
#include "me/object_pool.hpp"
#include "me/order_book.hpp"
#include "me/types.hpp"

#include <cassert>
#include <cstddef>
#include <expected>
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
    // The index is sized from the pool, exactly: an order that cannot be pooled
    // cannot rest, so `pool_capacity` is a hard bound on live index entries and
    // the index never needs to grow (D19).
    Engine(Price min_price, Price max_price, std::size_t pool_capacity)
        : book_(min_price, max_price, pool_capacity), pool_(pool_capacity) {}

    // Match what crosses, rest what remains. Trades are appended to `out`.
    // Yields the engine-assigned id, or the reason it was refused (D28).
    [[nodiscard]] std::expected<OrderId, RejectReason> apply(const NewOrder& cmd,
                                                            std::vector<Trade>& out) {
        if (const auto why = validate(cmd); why.has_value()) {
            emit(OrderRejected{.seq = next_seq_++, .reason = *why});
            return std::unexpected(*why);
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
        // D25.2 CORRECTS THIS. Reserving unconditionally rejected a marketable limit
        // order at pool exhaustion — an order that would have CONSUMED resting
        // liquidity and freed slots, which is exactly the order a venue wants at
        // capacity. Demonstrated: the identical size sent as a Market order traded
        // while the limit was refused. The D19 note above costs reserve-first as "one
        // acquire/release pair on the fully-filled path"; that is true in the common
        // case and silent about the boundary the change was about.
        //
        // So: reject early ONLY when the pool is empty AND the order could not have
        // traded anyway. Otherwise let it through — if it fills completely it never
        // needed a slot, and if a remainder survives it is cancelled below with the id
        // it was accepted under, which keeps the log foldable exactly as F4 requires.
        Order* slot = nullptr;
        if (cmd.type == OrderType::Limit) {
            slot = pool_.acquire();
            if (slot == nullptr && !can_match(cmd)) {
                emit(OrderRejected{.seq = next_seq_++, .reason = RejectReason::PoolExhausted});
                return std::unexpected(RejectReason::PoolExhausted);   // no id burned
            }
        }

        // D25.4 — fill() appends to the CALLER's vector, which can throw. `slot` was a
        // raw local with no cleanup on any path, so a bad_alloc there leaked the slot
        // permanently: check_invariants() went false and stayed false, and repeating it
        // drained the pool. Engine::apply had no exception-safety story at all.
        SlotGuard guard(pool_, slot);

        const SeqNum  arrival = next_seq_++;
        const OrderId id      = next_id_++;
        emit(OrderAccepted{.seq = arrival, .id = id, .side = cmd.side, .type = cmd.type,
                           .price = canonical_price(cmd), .quantity = cmd.quantity});

        Quantity remaining = cmd.quantity;
        fill(cmd, id, remaining, out);

        if (remaining == 0) {
            return id;                      // fully filled; guard returns any slot
        }

        // Anything that is not a Limit NEVER rests. Written as != Limit rather than
        // == Market so that no value outside the enumerators can reach the resting path
        // below, which is how D25.1 became a null-pointer write. validate() already
        // rejects such values; this is the second lock on the same door.
        if (cmd.type != OrderType::Limit) {
            emit(OrderCancelled{.seq = next_seq_++, .id = id,
                                .reason = CancelReason::NoLiquidity});
            return id;
        }

        // D25.2 — the pool was full on entry and this order was let through because it
        // could trade. A surviving remainder means every crossing maker was fully
        // consumed, and each of those was retired, and retiring returns a slot. So one
        // is free now BY CONSTRUCTION, and the remainder can rest after all.
        //
        // This is the half of D25.2 that is easy to miss: rejecting the order was the
        // visible bug, but cancelling its remainder would have been the same mistake
        // one step later — refusing a queue position that the order's own fill paid for.
        if (slot == nullptr) {
            slot = pool_.acquire();
            guard.adopt(slot);
        }

        // Expected unreachable, and not assumed. D8's rule is that the engine reports
        // rather than trusts: if the reasoning above is ever wrong, the order is
        // cancelled with the id it was accepted under, which keeps the log foldable.
        // The alternative is dereferencing a null pointer to prove a comment right.
        if (slot == nullptr) {
            emit(OrderCancelled{.seq = next_seq_++, .id = id,
                                .reason = CancelReason::PoolExhausted});
            return id;
        }

        Order* resting = slot;

        resting->id          = id;
        resting->side        = cmd.side;
        resting->type        = cmd.type;
        resting->price       = cmd.price;
        resting->quantity    = cmd.quantity;
        resting->remaining   = remaining;   // what survived the fill loop
        resting->entry_seq   = arrival;
        resting->participant = cmd.participant;

        // add() validates and can throw; the guard still owns the slot until it
        // returns, so an exception here returns the slot instead of leaking it.
        book_.add(resting);
        guard.dismiss();                    // the book owns it now
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

    // F9. `apply` appends to the CALLER's vector, so a sweep deeper than its
    // capacity reallocates — an unbounded allocation on the hot path, which is
    // the very thing D18 was rejected for. The bound is the pool's: every trade
    // but the last fully consumes a resting maker, and at most `capacity` can
    // rest. Reserve this and apply() cannot allocate. Same argument that sized
    // IdIndex (D19): the pool is what makes an unbounded-looking thing bounded.
    // Tight bound: every trade touches a distinct resting maker (all but possibly the
    // last are fully consumed), and at most `capacity` orders rest, so `capacity`
    // trades is the ceiling — the previous `+ 1` was slack, not safety.
    //
    // Two conditions, both real: `out` must be EMPTY on entry (apply APPENDS, so a
    // reused vector accumulates), and no allocating EventSink may be attached, since
    // that allocates per event regardless of this bound.
    [[nodiscard]] std::size_t max_trades_per_apply() const noexcept {
        return pool_.capacity();
    }

    // const only, deliberately. A mutable handle lets a caller add or remove
    // behind retire()'s back, which is the one path that keeps the book, the id
    // index and the pool in step (D14). Nothing outside Engine needs to mutate.
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
        // D25.5 — and an assert is NOT a fix, because NDEBUG is the build that ships
        // and the build that is measured. F6 recorded this as "now asserted" and left
        // the release running unconditionally: with remove() refusing, the pool freed
        // an order still linked into a level and still in the id index, and apply(Cancel)
        // returned TRUE while leaving a dangling pointer in both. Leaking a slot is
        // strictly better than handing out a live one twice.
        if (!removed) return;
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
        // D25.1 — reject values outside the enumerators BEFORE anything branches on
        // them. OrderType is `enum class : uint8_t`, so static_cast<OrderType>(2) is a
        // well-defined value, and five predicates across four phases disagreed about
        // it: it reserved no slot, skipped price validation, was not Market, and still
        // reached the resting path — a null-pointer WRITE under NDEBUG. scenario.hpp
        // casts an integer straight off a text log, so this is the external input path,
        // and the format exists to ingest third-party data.
        if (cmd.side != Side::Buy && cmd.side != Side::Sell) {
            return RejectReason::MalformedOrder;
        }
        if (cmd.type != OrderType::Limit && cmd.type != OrderType::Market) {
            return RejectReason::MalformedOrder;
        }
        if (cmd.quantity == 0) return RejectReason::InvalidQuantity;
        // D25.7 — PriceLevel caches a running SUM of quantity, and an unbounded
        // quantity wraps it. Worse, is_consistent() recomputes that sum with the same
        // wrapping arithmetic, so it agrees with the corrupted value: two orders of
        // 2^63 made depth_at() report ZERO while 2^64 rested, with every invariant
        // green. A checker that recomputes a value the way it was computed cannot see
        // an arithmetic fault in that computation.
        if (cmd.quantity > kMaxQuantity) return RejectReason::InvalidQuantity;
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
    // Returns the reserved slot on ANY exit that did not hand it to the book —
    // including an exception thrown out of the caller's vector inside fill(). See
    // D25.4; before this, `slot` was a raw local and the leak was permanent.
    class SlotGuard {
    public:
        SlotGuard(ObjectPool<Order>& pool, Order* slot) noexcept : pool_(pool), slot_(slot) {}
        ~SlotGuard() { if (slot_ != nullptr) pool_.release(slot_); }
        SlotGuard(const SlotGuard&)            = delete;
        SlotGuard& operator=(const SlotGuard&) = delete;
        void dismiss() noexcept { slot_ = nullptr; }
        // Take ownership of a slot acquired after construction (D25.2's retry).
        void adopt(Order* slot) noexcept { assert(slot_ == nullptr); slot_ = slot; }
    private:
        ObjectPool<Order>& pool_;
        Order*             slot_;
    };

    OrderId           next_id_  = 1;    // 0 is never issued; IdIndex uses it as EMPTY
    EventSink*        sink_     = nullptr;
};

} // namespace me
