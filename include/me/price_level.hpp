// me/price_level.hpp — one price, FIFO by arrival.
//
// Intrusive doubly-linked list: prev/next live inside Order, so one order is
// one object at one address, and holding an Order* IS holding its queue
// position. That is what makes O(1) cancel possible from Phase 4.
//
// Rationale in SYSTEM-DESIGN.md D9. .
#pragma once

#include "me/types.hpp"

#include <cassert>

namespace me {

class PriceLevel {
public:
    PriceLevel() = default;
    explicit PriceLevel(Price p) noexcept : price_(p) {}

    // Arrival: newest goes to the back. Precondition: o is in no other list.
    void push_back(Order* o) noexcept {
        assert(o != nullptr);

        o->prev = tail_;
        o->next = nullptr;

        if (tail_ != nullptr) {
            tail_->next = o;
        } else {
            head_ = o;                  // list was empty, so o is both ends
        }
        tail_ = o;

        total_quantity_ += o->remaining;
    }

    // THE single audited removal path. Every caller that takes an order out of
    // a level goes through here — fill-to-zero, cancel, amend, STP. Never
    // inline this surgery anywhere else (Blueprint §3.3).
    //
    // Precondition: o is in THIS list, and o->remaining still holds the amount
    // this level is counting. Subtracting happens here, so the caller must not
    // zero remaining first — see D9.
    void unlink(Order* o) noexcept {
        assert(o != nullptr);
        assert(total_quantity_ >= o->remaining);

        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            head_ = o->next;            // o was the head
        }

        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            tail_ = o->prev;            // o was the tail
        }

        total_quantity_ -= o->remaining;

        // Turns a use-after-unlink into a null dereference instead of a walk
        // into a list this order is no longer part of.
        o->prev = nullptr;
        o->next = nullptr;
    }

    // Partially fill the head in place. It KEEPS its queue position — it did
    // nothing to deserve losing it.
    //
    // This exists so quantity changes and list changes each happen in exactly
    // one place: a full consumption goes through unlink(), never through here,
    // which is what removes D9's ordering trap.
    void reduce_front(Quantity qty) noexcept {
        assert(head_ != nullptr);
        assert(qty > 0 && qty < head_->remaining &&
               "full consumption goes through unlink(), not reduce_front()");

        head_->remaining -= qty;
        total_quantity_  -= qty;
    }

    // Oldest resting order — fills come off the head, which is what makes time
    // priority structural rather than a rule to enforce.
    [[nodiscard]] Order* front() const noexcept { return head_; }

    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

    // Cached, never recomputed by walking: the walk is a pointer chase and this
    // is read on every incoming order. Invariant: == sum of remaining.
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }

    [[nodiscard]] Price price() const noexcept { return price_; }
    void set_price(Price p) noexcept { price_ = p; }

    // O(n). Tests and Phase 4's check_invariants(), never the hot path.
    // Checks: reachable both ways, entry_seq strictly increases head to tail
    // (invariant 5), and the cached total matches the walk (invariant 4).
    [[nodiscard]] bool is_consistent() const noexcept {
        if (head_ == nullptr || tail_ == nullptr) {
            return head_ == nullptr && tail_ == nullptr && total_quantity_ == 0;
        }
        if (head_->prev != nullptr || tail_->next != nullptr) {
            return false;
        }

        Quantity   sum  = 0;
        SeqNum     last = 0;
        const Order* prev = nullptr;
        for (const Order* o = head_; o != nullptr; prev = o, o = o->next) {
            if (o->prev != prev)                    return false;   // back-link broken
            if (prev != nullptr && o->entry_seq <= last) return false;   // FIFO violated
            sum  += o->remaining;
            last  = o->entry_seq;
        }
        return prev == tail_ && sum == total_quantity_;
    }

private:
    Order*   head_           = nullptr;
    Order*   tail_           = nullptr;
    Quantity total_quantity_ = 0;
    Price    price_          = 0;
};

} // namespace me
