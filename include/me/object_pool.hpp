// me/object_pool.hpp — pre-allocated slab + free-list.
//
// Fixed-capacity recycler for T. Removes the allocator from the hot path: its
// worst case is unbounded, and unbounded is what disqualifies it.
//
// Rationale for every choice here lives in SYSTEM-DESIGN.md D5 and D8.
#pragma once

#include "me/asan.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace me {

template <typename T>
class ObjectPool {
public:
    using Index = std::uint32_t;

    static constexpr Index kNil   = std::numeric_limits<Index>::max();       // end of list
    static constexpr Index kInUse = std::numeric_limits<Index>::max() - 1;   // handed out
    static constexpr std::size_t kMaxCapacity = kInUse;

    static_assert(std::is_default_constructible_v<T>);
    static_assert(std::is_copy_assignable_v<T> || std::is_move_assignable_v<T>);

    explicit ObjectPool(std::size_t capacity)
        : slab_(checked_capacity(capacity)), next_free_(capacity, kNil) {
        // Threaded back-to-front so the head lands on index 0 and a fresh pool
        // hands out slots in ascending address order.
        free_head_ = kNil;
        for (std::size_t n = capacity; n-- > 0;) {
            const Index i = static_cast<Index>(n);
            next_free_[i] = free_head_;
            free_head_    = i;
            poison_payload(slab_.data() + i);
        }
    }

    // The pool hands out interior pointers to its own storage, so "which pool
    // owns this slot" must have exactly one answer. See D5.
    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&)                 = delete;
    ObjectPool& operator=(ObjectPool&&)      = delete;

    ~ObjectPool() {
        // Required: poison lives in ASan's shadow map, not the memory, so it
        // outlives the slab and would fire on innocent code later.
        for (std::size_t i = 0; i < slab_.size(); ++i) {
            unpoison_payload(slab_.data() + i);
        }
    }

    // nullptr when exhausted. The slot is value-initialised (D8).
    [[nodiscard]] T* acquire() noexcept {
        if (free_head_ == kNil) {
            return nullptr;
        }

        const Index i = free_head_;
        free_head_    = next_free_[i];
        next_free_[i] = kInUse;
        ++in_use_;

        T* slot = slab_.data() + i;
        unpoison_payload(slot);
        *slot = T{};
        return slot;
    }

    // false = null, foreign, mis-aligned, or already free. Nothing is mutated in
    // those cases. Both checks are unconditional, not assert: see D8.
    bool release(T* slot) noexcept {
        if (slot == nullptr) {
            return false;
        }

        const Index i = index_of(slot);
        if (i == kNil) {
            return false;                   // else next_free_[kNil] is a wild write
        }
        if (next_free_[i] != kInUse) {
            return false;                   // double release would splice a cycle
        }

        poison_payload(slot);               // only after the pointer is proven ours
        next_free_[i] = free_head_;
        free_head_    = i;
        --in_use_;
        return true;
    }

    [[nodiscard]] std::size_t in_use()    const noexcept { return in_use_; }
    [[nodiscard]] std::size_t available() const noexcept { return slab_.size() - in_use_; }
    [[nodiscard]] std::size_t capacity()  const noexcept { return slab_.size(); }

    // Not empty(): PriceLevel::empty() means "holds nothing", this is its opposite.
    [[nodiscard]] bool exhausted() const noexcept { return free_head_ == kNil; }

    // D27 — test hook, and it earns its keep. Mutation testing showed that every
    // clause of is_consistent() could be deleted with no test noticing, because
    // several of them guard PRIVATE state that no public API can corrupt. The suite's
    // own rule is "a checker that never fails proves nothing"; without this the rule
    // could not be applied here. Declared, never defined outside the test build.
    friend struct Probe;

    // Acyclic, and exactly available() long. O(capacity) — tests and Phase 4's
    // check_invariants(), never the hot path.
    [[nodiscard]] bool free_list_is_consistent() const noexcept {
        std::size_t seen = 0;
        Index cursor = free_head_;
        while (cursor != kNil) {
            if (cursor >= capacity())          return false;   // before indexing, not after
            if (next_free_[cursor] == kInUse)  return false;
            if (++seen > capacity())           return false;   // cycle
            cursor = next_free_[cursor];
        }
        return seen == available();
    }

private:
    static std::size_t checked_capacity(std::size_t n) {
        // In the init list, so it runs before the vectors are sized.
        if (n > kMaxCapacity) {
            throw std::length_error("ObjectPool capacity exceeds the 32-bit index space");
        }
        return n;
    }

    // kNil unless `slot` is a live element boundary of this slab.
    // uintptr_t because comparing pointers into different objects is unspecified;
    // the % check because a range test alone accepts interior pointers, and
    // subtracting those is UB that neither ASan nor UBSan diagnoses.
    [[nodiscard]] Index index_of(const T* slot) const noexcept {
        const auto base = reinterpret_cast<std::uintptr_t>(slab_.data());
        const auto addr = reinterpret_cast<std::uintptr_t>(slot);
        if (addr < base) {
            return kNil;
        }
        const std::uintptr_t offset = addr - base;
        if (offset % sizeof(T) != 0) {
            return kNil;
        }
        const std::uintptr_t idx = offset / sizeof(T);
        return idx < slab_.size() ? static_cast<Index>(idx) : kNil;
    }

    // Poison whole aligned 8-byte granules or it is silently ignored (asan.hpp).
    static constexpr bool kPoisonable = (alignof(T) % kAsanGranule == 0);

    static_assert(!ME_HAS_ASAN || kPoisonable,
                  "ObjectPool<T>: alignof(T) < 8, so poisoning would be silently ignored "
                  "and pool discipline for this T is UNVERIFIED.");

    static void poison_payload([[maybe_unused]] T* slot) noexcept {
        if constexpr (kPoisonable) { ME_POISON(slot, granule_floor(sizeof(T))); }
    }

    static void unpoison_payload([[maybe_unused]] T* slot) noexcept {
        if constexpr (kPoisonable) { ME_UNPOISON(slot, granule_floor(sizeof(T))); }
    }

    std::vector<T>     slab_;        // one allocation, made once
    std::vector<Index> next_free_;   // free-list links, separate, never poisoned
    Index              free_head_ = kNil;
    std::size_t        in_use_    = 0;
};

} // namespace me
