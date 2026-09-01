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
constexpr std::size_t kFill = 200'000;   // resting depth before the measured loop
constexpr std::size_t kOps  = 2'000'000;

} // namespace

int main(int argc, char** argv) {
    if (ME_HAS_ASAN) { std::fprintf(stderr, "refusing: sanitizer build\n"); return 2; }
#ifndef NDEBUG
    std::fprintf(stderr, "refusing: assertions enabled\n"); return 2;
#endif
    const std::string mode = (argc > 1) ? argv[1] : "rest";

    Engine eng(9'000, 11'000, 1 << 21);
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
        const OrderId id = eng.apply(NewOrder{
            .side = (i % 2 == 0) ? Side::Buy : Side::Sell,
            .type = OrderType::Limit,
            // Bids strictly below asks, so this fill never trades: the point is
            // depth, not activity.
            .price = static_cast<Price>((i % 2 == 0) ? kLo - 1 - (i % 50)
                                                     : kHi + 1 + (i % 50)),
            .quantity = static_cast<Quantity>(qty_of(rng)),
            .participant = 1}, trades);
        if (id != Engine::kRejected) live.push_back(id);
    }

    // Reset AFTER building the book: we are measuring the operation, not the fill.
    count::news = 0;
    count::dels = 0;
    std::size_t ops_done = 0;

    std::size_t sink = 0;

    if (mode == "cancel") {
        // Cancel orders that exist. Each is a hash lookup, an unlink, an index
        // erase, a pool release, and possibly a cursor advance.
        std::size_t i = 0;
        for (std::size_t n = 0; n < kOps && i < live.size(); ++n, ++i) {
            sink += eng.apply(Cancel{live[i]}) ? 1u : 0u;
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
            eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                               .price = static_cast<Price>(kHi + 60),
                               .quantity = 1, .participant = 1}, trades);
            sink += trades.size();
            ++ops_done;
        }
    } else {
        // rest: never crosses, so it is add + push_back + index insert.
        for (std::size_t n = 0; n < kOps; ++n) {
            trades.clear();
            sink += eng.apply(NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                                       .price = static_cast<Price>(price_of(rng) - 300),
                                       .quantity = 1, .participant = 1}, trades);
            ++ops_done;
        }
    }

    std::printf("mode=%-12s ops=%-9zu new=%-9zu delete=%-9zu  per-op: new=%.2f delete=%.2f\n",
                mode.c_str(), ops_done, count::news, count::dels,
                static_cast<double>(count::news) / static_cast<double>(ops_done),
                static_cast<double>(count::dels) / static_cast<double>(ops_done));
    if (trades.capacity() != trade_cap0) {
        std::fprintf(stderr,
                     "INVALID: the trade vector grew %zu -> %zu inside the measured "
                     "loop, so the counts above include the caller's allocations\n",
                     trade_cap0, trades.capacity());
        return 3;
    }
    (void)sink;
    return 0;
}
