// bench/profile.cpp — one operation kind, in a tight loop, for perf to sample.
//
// Deliberately has no timing, no percentiles and no sorting: those are
// bench_latency's job, and their cost would pollute the counters. This binary
// exists so `perf stat` can be pointed at ONE code path and the difference
// between kinds read in cache misses rather than guessed at.
//
//   perf stat -e cycles,instructions,cache-misses,branch-misses ./bench_profile cancel
//
// Rationale in SYSTEM-DESIGN.md D19 (the aligned-operator-new blind spot it exists
// to close is F3).
#include "me/asan.hpp"
#include "me/engine.hpp"
#include "me/types.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// Count heap traffic. The pool exists to keep the allocator off the hot path;
// this is how we find out whether something else put it back.
namespace count { std::size_t news = 0, dels = 0; }

void* operator new(std::size_t n) {
    ++count::news;
    void* p = std::malloc(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { ++count::dels; std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ++count::dels; std::free(p); }

// F3: the aligned overloads. Order is alignas(64), so EVERY Order allocation
// went through these and was invisible to the counter above — the instrument
// built to police hot-path allocation could not see the hot path's own type.
void* operator new(std::size_t n, std::align_val_t a) {
    ++count::news;
    void* p = std::aligned_alloc(static_cast<std::size_t>(a), n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p, std::align_val_t) noexcept { ++count::dels; std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ++count::dels; std::free(p); }

using namespace me;

namespace {

constexpr Price       kLo   = 9'900;
constexpr Price       kHi   = 10'100;
// D27/1.16 and 1.17. Two defects, both silent:
//   · kFill 200k + kOps 2M against a pool of 2^21 meant the LAST 5.1% of "rest"
//     operations were PoolExhausted rejects — a two-branch early return, not the
//     add + push_back + index-insert the mode claims to isolate. The binary refuses
//     to report if the trade vector grew and had no equivalent check for this.
//   · "cancel" ran `n < kOps && i < live.size()`, so it did 200,000 operations while
//     every other mode did 2,000,000 and the header claimed all modes run against the
//     same shape. A tenth of the samples, undisclosed.
// Now: equal op counts everywhere, and a pool with room for fill + rest with margin.
constexpr std::size_t kFillDefault = 1'000'000;   // resting depth before the measured loop
constexpr std::size_t kOpsDefault  = 1'000'000;

} // namespace

int main(int argc, char** argv) {
    if (ME_HAS_ASAN) { std::fprintf(stderr, "refusing: sanitizer build\n"); return 2; }
#ifndef NDEBUG
    std::fprintf(stderr, "refusing: assertions enabled\n"); return 2;
#endif
    const std::string mode = (argc > 1) ? argv[1] : "rest";
    // Depth and op count are arguments so the SHAPE of a cost can be measured, not
    // just its value at one size. A number that is fine at one depth and quadratic at
    // another is not the same finding as a number that is simply large.
    const std::size_t kFill = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : kFillDefault;
    const std::size_t kOps  = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : kOpsDefault;

    Engine eng(9'000, 11'000, 1 << 22);
    // F9: a reallocation here would be an allocation inside the measured loop,
    // which is precisely what this binary exists to detect. Reserve generously
    // and VERIFY afterwards rather than assuming — 64 was a guess, and a guess
    // that holds is still a guess.
    std::vector<Trade> trades;
    trades.reserve(4096);
    const std::size_t trade_cap0 = trades.capacity();

    std::mt19937 rng(11u);
    std::uniform_int_distribution<int> price_of(kLo, kHi);
    std::uniform_int_distribution<int> qty_of(1, 500);

    // Build a deep book first. Every mode measures against the same shape, so a
    // difference between modes is the operation and not the book it ran against.
    std::vector<OrderId> live;
    live.reserve(kFill);
    for (std::size_t i = 0; i < kFill; ++i) {
        trades.clear();
        const auto id = eng.apply(NewOrder{
            .side = (i % 2 == 0) ? Side::Buy : Side::Sell,
            .type = OrderType::Limit,
            // Bids strictly below asks, so this fill never trades: the point is
            // depth, not activity.
            .price = static_cast<Price>((i % 2 == 0) ? kLo - 1 - (i % 50)
                                                     : kHi + 1 + (i % 50)),
            .quantity = static_cast<Quantity>(qty_of(rng)),
            .participant = 1}, trades);
        if (id.has_value()) live.push_back(*id);
    }

    // Precomputed BEFORE the counters are reset, for the same reason the book is built
    // first: the measurement must contain the operation and nothing else. Doing this
    // inside the loop put an mt19937 draw (a 624-word state) next to the very
    // cache-miss counter this binary exists to have sampled (D27/R14); doing it after
    // the reset counted the vector's own allocation as the engine's.
    std::vector<Price> prices;
    prices.reserve(kOps);
    for (std::size_t n = 0; n < kOps; ++n) {
        prices.push_back(static_cast<Price>(price_of(rng) - 300));
    }

    // Reset AFTER building the book: we are measuring the operation, not the fill.
    count::news = 0;
    count::dels = 0;
    std::size_t ops_done = 0;

    std::size_t sink     = 0;
    std::size_t rejected = 0;

    if (mode == "none") {
        // Fill only, measured loop skipped. perf counts the WHOLE process, and the
        // fill is the same order of magnitude as the measured loop, so a perf stat of
        // any mode is roughly half setup. Running this mode gives the setup's counters
        // on their own, to be subtracted. Approximate (aggregate subtraction, not
        // per-region counting) and stated as such wherever the numbers are used.
        ops_done = 1;                          // avoid a divide by zero in the report
    } else if (mode == "cancel") {
        // Cancel orders that exist. Each is a hash lookup, an unlink, an index
        // erase, a pool release, and possibly a cursor advance.
        // D27/R15 — `live` is in insertion order, i.e. strictly ascending ids, and
        // IdIndex hashes by identity. Walking it in order is a perfectly sequential,
        // maximally cache-friendly probe pattern that no real cancel flow resembles.
        // bench/baseline.cpp randomises cancel order for exactly this reason.
        std::shuffle(live.begin(), live.end(), rng);
        for (std::size_t n = 0; n < kOps && n < live.size(); ++n) {
            sink += eng.apply(Cancel{live[n]}) ? 1u : 0u;
            ++ops_done;
        }
    } else if (mode == "cancel_miss") {
        // Cancel ids that do not exist: isolates the LOOKUP alone.
        for (std::size_t n = 0; n < kOps; ++n) {
            sink += eng.apply(Cancel{static_cast<OrderId>(50'000'000 + n)}) ? 1u : 0u;
            ++ops_done;
        }
    } else if (mode == "trade") {
        // Cross the spread every time.
        for (std::size_t n = 0; n < kOps; ++n) {
            trades.clear();
            (void)eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                               .price = static_cast<Price>(kHi + 60),
                               .quantity = 1, .participant = 1}, trades);
            sink += trades.size();
            ++ops_done;
        }
    } else {
        // rest: never crosses, so it is add + push_back + index insert.
        //
        // D27/R14 — the price draw used to happen INSIDE the loop, and only in this
        // mode. An mt19937 draw touches a 624-word state, which pollutes cache-misses:
        // one of the exact counters this binary exists to have sampled by perf, and
        // the stated reason it carries no timing code. Precomputed instead.
        for (std::size_t n = 0; n < kOps; ++n) {
            trades.clear();
            const auto id = eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                               .price = prices[n],
                                                  .quantity = 1, .participant = 1}, trades);
            if (!id.has_value()) ++rejected;
            sink += id.value_or(0);
            ++ops_done;
        }
    }

    // D27/1.16 — refuse if the pool ran out mid-measurement. The mode would still
    // print a plausible number while a growing share of its samples were a two-branch
    // early return rather than the operation named on the line.
    if (rejected != 0) {
        std::fprintf(stderr,
                     "INVALID: %zu of %zu operations were rejected (pool exhausted), so "
                     "these samples are not all the operation this mode claims\n",
                     rejected, ops_done);
        return 3;
    }

    // D26 — check BEFORE printing. This used to report the results line and flag the
    // problem afterwards, while the README claimed it would "abort rather than report".
    // A rig that prints a number and then says the number is wrong has already lost:
    // the number is what gets copied.
    if (trades.capacity() != trade_cap0) {
        std::fprintf(stderr,
                     "INVALID: the trade vector grew %zu -> %zu inside the measured "
                     "loop, so these counts include the caller's allocations\n",
                     trade_cap0, trades.capacity());
        return 3;
    }

    std::printf("mode=%-12s ops=%-9zu new=%-9zu delete=%-9zu  per-op: new=%.2f delete=%.2f\n",
                mode.c_str(), ops_done, count::news, count::dels,
                static_cast<double>(count::news) / static_cast<double>(ops_done),
                static_cast<double>(count::dels) / static_cast<double>(ops_done));
    (void)sink;
    return 0;
}
