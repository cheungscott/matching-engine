// me/price_level.hpp — one price, FIFO by arrival. Phase 1.
//
// ===========================================================================
//  THE BODIES ARE YOURS. Signatures from Blueprint §3.5.
// ===========================================================================
//
// This is an INTRUSIVE doubly-linked list: the prev/next pointers live inside
// Order itself, not in a separate node wrapper. Consequences:
//   - no per-node allocation
//   - one object = one memory location
//   - holding an Order* IS holding its position in the container, which is what
//     makes O(1) cancel possible once Phase 4 can find the order by id
// The tax is that you own every pointer update by hand.
//
// WHY FIFO: time priority rewards showing your hand early and honestly. Without
// it there is no incentive to be first in the queue and no penalty for
// flickering quotes in and out. `entry_seq` IS the clock — there is no other.
//
// >>> THE ONE RULE THAT SAVES YOU A DEBUGGING WEEK (Blueprint §3.3) <<<
// unlink() is a SINGLE AUDITED FUNCTION. Never inline pointer surgery into the
// match loop, or cancel, or amend, or self-trade prevention. Every removal path
// — fill-to-zero, cancel, amend-as-cancel-replace — goes through this one
// function. Get the head/tail special cases wrong in one place instead of five.
#pragma once

#include "me/types.hpp"

namespace me {

class PriceLevel {
public:
    PriceLevel() = default;
    explicit PriceLevel(Price p) : price_(p) {}

    // Arrival. Goes to the BACK (it is the newest). Maintains total_quantity_.
    void push_back(Order* /*o*/) {
        // TODO(you)
    }

    // O(1) removal. PRECONDITION: o is currently in THIS list.
    // Handle: o is head, o is tail, o is both, o is neither.
    void unlink(Order* /*o*/) {
        // TODO(you)
    }

    // Oldest resting order, or nullptr when empty. Fills come off the head.
    [[nodiscard]] Order* front() const noexcept {
        return nullptr;   // TODO(you)
    }

    [[nodiscard]] bool empty() const noexcept {
        return true;      // TODO(you)
    }

    // CACHED, never recomputed by walking the list — that walk is a pointer
    // chase and this is read on every incoming order.
    // Invariant: == sum of `remaining` over the list.
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }

    [[nodiscard]] Price price() const noexcept { return price_; }

    void set_price(Price p) noexcept { price_ = p; }

private:
    Order*   head_           = nullptr;
    Order*   tail_           = nullptr;
    Quantity total_quantity_ = 0;
    Price    price_          = 0;
};

} // namespace me
