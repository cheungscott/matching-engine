// me/object_pool.hpp — pre-allocated slab + free-list. Phase 1.
//
// ===========================================================================
//  Signatures exist so the tests compile; every one of them is a stub
//  until tests/phase1_tests.cpp passes.
// ===========================================================================
//
// WHY A POOL AT ALL (three independent reasons):
//   1. Allocator latency is variable and occasionally UNBOUNDED (internal locks,
//      page faults on fresh memory). Unbounded is what disqualifies it, not slow.
//   2. Allocations scatter related objects, destroying locality for every later
//      traversal of the book.
//   3. Churn fragments the heap.
// A pool converts an unbounded-latency operation into a pointer bump.
//
// OPEN DESIGN DECISIONS (log them in SYSTEM-DESIGN.md):
//   - Fixed capacity, or grow on exhaustion? Blueprint §10g lists this open.
//     Growing reintroduces an unbounded allocation on the hot path; rejecting
//     means the venue can refuse an order because it is full.
//   - What does acquire() do when empty? nullptr, throw, or assert?
//   - Where does the free-list link live? See the ASan rules in me/asan.hpp —
//     if the link sits inside the poisoned region, walking the free-list
//     trips the sanitizer.
//
// THE INVARIANT THIS EXISTS TO UPHOLD (Blueprint invariant 7):
//   Every reachable Order* is a live acquired slot; every unlinked order returns
//   to the pool EXACTLY ONCE. No double-free, no leak.
#pragma once

#include "me/asan.hpp"

#include <cstddef>
#include <vector>

namespace me {

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : slab_(capacity) {
        // TODO: thread every slot onto the free-list, then poison the
        // payload of each one (aligned granules only; the link stays readable).
    }

    // Hand out a slot. Contents are unspecified — the caller initialises.
    [[nodiscard]] T* acquire() {
        // TODO: pop the free-list head, unpoison the payload, return it.
        return nullptr;
    }

    // Return a slot. After this call the caller's pointer is DEAD; touching it
    // is the bug ASan is here to catch.
    void release(T* /*slot*/) {
        // TODO: push onto the free-list and poison the payload.
    }

    // Slots currently handed out. Tests use this to prove release() recycles
    // rather than leaking.
    [[nodiscard]] std::size_t in_use() const noexcept {
        return 0;   // TODO
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slab_.size(); }

private:
    std::vector<T> slab_;      // one contiguous allocation, made once
    T*             free_head_ = nullptr;
    std::size_t    in_use_    = 0;
};

} // namespace me
