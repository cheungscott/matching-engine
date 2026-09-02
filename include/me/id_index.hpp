// me/id_index.hpp — fixed-capacity id -> Order* map. Allocates exactly once.
//
// Rationale in SYSTEM-DESIGN.md D19. The short version:
//
//   std::unordered_map  one BOUNDED allocation per insert, one free per erase.
//   std::vector by id   allocations are rare but UNBOUNDED — measured at 8.1ms
//                       for a single apply() at 2M orders, because growth is
//                       O(ids ever issued) and ids never stop.
//   this                zero allocations after construction, bounded memory.
//
// "Unbounded is what disqualifies it, not slow" is the pool's founding rule, and
// both previous attempts broke it — the second one worse than the first.
//
// The key observation: **at most `capacity` orders can rest at once**, because
// the pool says so. Live entries are bounded even though ids are not, so the
// table can be sized once from the pool and never grow.
// See WORKING-RULES.md for the mode rule.
#pragma once

#include "me/types.hpp"

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace me {

class IdIndex {
public:
    // Sized to the pool: load factor can never exceed 0.5, because the pool
    // cannot hand out more than `capacity` slots at once.
    explicit IdIndex(std::size_t capacity)
        : table_(std::bit_ceil(std::max<std::size_t>(capacity, 1) * 2)),
          mask_(table_.size() - 1),
          shift_(64 - static_cast<unsigned>(std::countr_zero(table_.size()))) {}

    // True when another insert would breach the load factor the probe loops rely on.
    // Public so the CALLER can refuse before mutating anything — see OrderBook::add.
    [[nodiscard]] bool full() const noexcept { return (count_ + 1) * 2 > table_.size(); }

    // D25.9 — every probe loop is bounded by the table size.
    //
    // These were `for(;;)`, terminating only because an empty slot always exists, which
    // held only because of an `assert` on the load factor — and asserts are deleted in
    // the build that ships. With a full table, a lookup of a missing id SPUN FOREVER.
    // A hang is the worst failure a trading process has: no crash, no core, no log, and
    // a watchdog is the only thing that ever notices. Bounding the loop costs one
    // comparison per probe and converts an unbounded hang into a miss.
    [[nodiscard]] Order* find(OrderId id) const noexcept {
        if (id == kEmpty) return nullptr;
        std::size_t i = home(id);
        for (std::size_t probes = 0; probes < table_.size(); ++probes, i = (i + 1) & mask_) {
            if (table_[i].id == kEmpty) return nullptr;
            if (table_[i].id == id)     return table_[i].node;
        }
        return nullptr;
    }

    // Precondition, enforced by the caller via full(): the table has room. Kept noexcept
    // so OrderBook::add cannot throw AFTER it has already linked the order into a level.
    void insert(OrderId id, Order* node) noexcept {
        assert(id != kEmpty && node != nullptr);
        assert(!full() && "IdIndex::insert on a full table — caller skipped full()");

        std::size_t i = home(id);
        for (std::size_t probes = 0; probes < table_.size(); ++probes, i = (i + 1) & mask_) {
            if (table_[i].id == kEmpty) {
                table_[i] = Slot{id, node};
                ++count_;
                return;
            }
            assert(table_[i].id != id && "id reused while still resting");
        }
        assert(false && "IdIndex::insert found no free slot");
    }

    bool erase(OrderId id) noexcept {
        if (id == kEmpty) return false;
        std::size_t i = home(id);
        for (std::size_t probes = 0; probes < table_.size(); ++probes, i = (i + 1) & mask_) {
            if (table_[i].id == kEmpty) return false;
            if (table_[i].id == id) {
                erase_at(i);
                --count_;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t size()     const noexcept { return count_; }
    // NOTE the unit: SLOTS in the table, which is bit_ceil(orders * 2) — not the order
    // count the constructor takes, and not what ObjectPool::capacity() means.
    [[nodiscard]] std::size_t slot_count() const noexcept { return table_.size(); }

    // Tests and check_invariants(): count live entries by walking the table, so
    // the counter can be checked against the structure rather than trusted.
    [[nodiscard]] std::size_t count_live() const noexcept {
        std::size_t n = 0;
        for (const Slot& s : table_) {
            if (s.id != kEmpty) ++n;
        }
        return n;
    }

    // D27 test hook — see tests/mutation.hpp. count_ has to be corruptible for
    // is_consistent()'s cardinality clause to have ever been shown to fire.
    friend struct Probe;

private:
    struct Slot {
        OrderId id   = kEmpty;
        Order*  node = nullptr;
    };

    // 0 is never a valid order id — Engine reserves it as kRejected — so it is
    // free to mean "empty" without a separate occupancy bit.
    static constexpr OrderId kEmpty = 0;

    // IDENTITY hash, deliberately. Ids are engine-assigned and strictly
    // increasing, so masking spreads them perfectly across buckets AND keeps
    // FIBONACCI HASHING, and the reason is D28.
    //
    // This was `id & mask_`, chosen because engine ids are strictly increasing so
    // masking "distributes them perfectly AND keeps recently-issued ids adjacent".
    // Both halves are true and the conclusion was backwards: perfect adjacency is
    // MAXIMAL CLUSTERING. Every live entry sat in one contiguous run of slots, and
    // linear probing stops at the first EMPTY slot — so any lookup whose home landed
    // inside that run had to walk to the end of it.
    //
    // Measured, miss lookups, live entries in one contiguous block:
    //     10,000 live ->     6,172 ns      320,000 live ->   241,035 ns
    // against 1.1 ns for a miss landing outside the block. Linear in the number of
    // resting orders, and 241 MICROSECONDS for a single cancel.
    //
    // Reachable from the public API with nothing malformed: 60 ns -> 1,793 ns with
    // only 3,000 resting orders. The entry above said it was "safe because ids are
    // never client-supplied" — but ids only need to ALIAS into the block, and the
    // engine itself starts issuing such ids as soon as next_id_ passes the table
    // size, which any real session does in minutes.
    //
    // This is D18's rule again, inside the structure D19 built to satisfy it:
    // unbounded is what disqualifies a design, not slow. The locality that was
    // traded away was never measured to be worth anything; the tail it created was
    // measured and is unbounded in the book's depth.
    //
    // BLOCK-SCATTER, which keeps both properties instead of trading one for the other.
    //
    // Plain Fibonacci hashing fixes the tail and costs 2-3x on the common path, because
    // the table is sized for POOL CAPACITY while a book usually holds far fewer orders:
    // scattering a few thousand live entries over a 2M-slot (33 MB) table makes every
    // lookup a cache miss. Identity hashing dodged that by accident, packing everything
    // into one small contiguous region that fits in L2 — the same clustering that
    // created the unbounded tail.
    //
    // So: scatter the BLOCK, keep ids adjacent WITHIN it. Consecutive ids share a
    // 64-slot block (16 cache lines, allocated together and touched together), and
    // successive blocks land far apart, so no run of live entries can grow past a
    // block or two. Locality is bounded below by the block; probe length is bounded
    // above by it.
    [[nodiscard]] std::size_t home(OrderId id) const noexcept {
        constexpr std::uint64_t kPhi   = 0x9E37'79B9'7F4A'7C15ULL;
        constexpr std::uint64_t kBlock = 64;
        const std::uint64_t     v      = static_cast<std::uint64_t>(id);
        const std::uint64_t     blk    = ((v / kBlock) * kPhi) >> (shift_ + 6);
        return static_cast<std::size_t>(((blk * kBlock) + (v % kBlock)) & mask_);
    }

    // Backward-shift deletion. Tombstones would be simpler, but they accumulate
    // and degrade every later probe — in a process that runs all day, a
    // tombstone-based table gets permanently slower. This keeps the table clean
    // by moving any element whose probe chain would be broken by the hole.
    void erase_at(std::size_t i) noexcept {
        for (;;) {
            table_[i] = Slot{};
            std::size_t j = i;
            for (;;) {
                j = (j + 1) & mask_;
                if (table_[j].id == kEmpty) return;      // chain ends; done
                const std::size_t k = home(table_[j].id);
                // Can table_[j] move back into the hole at i? Only if its ideal
                // position k does NOT lie cyclically within (i, j].
                const bool cannot_move = (i <= j) ? (i < k && k <= j)
                                                  : (i < k || k <= j);
                if (!cannot_move) break;
            }
            table_[i] = table_[j];
            i = j;
        }
    }

    std::vector<Slot> table_;      // one allocation, at construction, never grows
    std::size_t       mask_ = 0;
    std::size_t       count_ = 0;
    unsigned          shift_ = 0;     // 64 - log2(table size), for the Fibonacci hash
};

} // namespace me
