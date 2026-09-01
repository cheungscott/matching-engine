// bench/baseline.cpp — the engine against the oracle it is verified by.
//
// D6 folded this in as a "cheap win" and it never got built: point the Phase 10
// rig at NaiveBook as a PERFORMANCE baseline, not only a correctness one. That
// converts two claims the README makes from asserted to measured:
//
//   "a tree is O(log n) pointer chasing; an array index is arithmetic"
//   "the allocator's worst case is unbounded"
//
// NaiveBook is the honest strawman for both: std::map per side (a tree), a
// std::vector of resting orders per level, and an O(n) scan to cancel. It shares
// no code with the engine, which is what made it a usable oracle and is what
// makes it a usable baseline.
//
// The number that matters is not the ratio at one size — it is how the ratio
// MOVES with depth. A constant factor is an implementation detail; a growing one
// is the complexity claim. Rationale in SYSTEM-DESIGN.md D24.
#include "me/engine.hpp"
#include "me/asan.hpp"
#include "naive_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#endif
#include <vector>

using namespace me;

namespace {

// Same rdtscp pair bench_latency uses; see D17 for why not steady_clock.
inline std::uint64_t tsc_now() noexcept {
#if defined(__x86_64__)
    unsigned aux;
    _mm_lfence();
    return __rdtscp(&aux);
#else
    return 0;
#endif
}

double tsc_ghz() {
    const auto t0 = std::chrono::steady_clock::now();
    const auto c0 = tsc_now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) { }
    const auto c1 = tsc_now();
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / secs / 1e9;
}

constexpr Price kMin = 9'000;
constexpr Price kMax = 11'000;
constexpr Price kLo  = 9'900;
constexpr Price kHi  = 10'100;

struct Command {
    bool     is_cancel = false;
    OrderId  cancel_id = 0;
    NewOrder order{};
};

// Resting-heavy, with cancels of orders that actually exist. Prices sit strictly
// inside one side's band so the stream builds DEPTH rather than trading it away —
// depth is the variable both claims are about.
std::vector<Command> make_stream(std::uint32_t seed, std::size_t depth, std::size_t cancels) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price_of(kLo, kHi);
    std::uniform_int_distribution<int> qty_of(1, 500);

    std::vector<Command> out;
    out.reserve(depth + cancels);

    for (std::size_t i = 0; i < depth; ++i) {
        Command c;
        c.order = NewOrder{.side = Side::Buy, .type = OrderType::Limit,
                           .price = static_cast<Price>(price_of(rng) - 300),
                           .quantity = static_cast<Quantity>(qty_of(rng)), .participant = 1};
        out.push_back(c);
    }
    // Cancel ids spread across the whole resting population, so the naive scan
    // pays its average rather than always hitting the front.
    std::uniform_int_distribution<std::size_t> id_of(1, depth);
    for (std::size_t i = 0; i < cancels; ++i) {
        Command c;
        c.is_cancel = true;
        c.cancel_id = static_cast<OrderId>(id_of(rng));
        out.push_back(c);
    }
    return out;
}

struct Timing {
    double build_ns_per_op = 0;
    double cancel_ns_per_op = 0;
};

// D26 — the rig repeats itself now.
//
// This used to run each depth ONCE per invocation, and "medians of 3 runs" was
// something done by hand at the keyboard and recorded nowhere. The headline it
// produced (92x) was the median of 54.8 / 92.9 / 95.6 — arithmetically right, and a
// point estimate that should never have been quoted from a sample with that spread.
// An independent re-run got 74-87x and could not reproduce it.
//
// bench_latency's numbers held up better under the same scrutiny for exactly one
// reason: it does its 5-run median INSIDE the binary. Methodology that lives in the
// operator's head is not methodology, so this now does the same and prints the spread
// alongside the median rather than leaving the reader to assume there isn't one.
constexpr int kReps = 5;

double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <class Book>
Timing run(Book& book, const std::vector<Command>& cmds, std::size_t depth, double ghz) {
    std::vector<Trade> trades;
    trades.reserve(4096);

    const auto b0 = tsc_now();
    for (std::size_t i = 0; i < depth; ++i) {
        trades.clear();
        book.apply(cmds[i].order, trades);
    }
    const auto b1 = tsc_now();

    const auto c0 = tsc_now();
    for (std::size_t i = depth; i < cmds.size(); ++i) {
        book.cancel(cmds[i].cancel_id);
    }
    const auto c1 = tsc_now();

    const std::size_t ncancel = cmds.size() - depth;
    return Timing{
        static_cast<double>(b1 - b0) / static_cast<double>(depth) / ghz,
        static_cast<double>(c1 - c0) / static_cast<double>(ncancel) / ghz,
    };
}

// Engine spells cancel differently from NaiveBook; one shim rather than two
// copies of the timing loop.
struct EngineAdapter {
    Engine eng;
    explicit EngineAdapter(std::size_t cap) : eng(kMin, kMax, cap) {}
    OrderId apply(const NewOrder& c, std::vector<Trade>& out) { return eng.apply(c, out); }
    bool    cancel(OrderId id) { return eng.apply(Cancel{id}); }
};

} // namespace

int main() {
    if (ME_HAS_ASAN) { std::fprintf(stderr, "refusing: sanitizer build\n"); return 2; }
#ifndef NDEBUG
    std::fprintf(stderr, "refusing: assertions enabled\n"); return 2;
#endif

    const double ghz = tsc_ghz();

    // Warm-up, discarded. An earlier invocation reported naive add at 71,652
    // ns/op on the first row — a 4,159x "result" that was pure first-touch
    // artefact, and it did not reproduce. A benchmark's first measurement is
    // always the coldest; reporting it is how a rig manufactures a spectacular
    // number nobody can reproduce. bench_latency already warms up for the same
    // reason (D17).
    {
        const auto warm = make_stream(1u, 2'000, 1'000);
        EngineAdapter    we(1u << 17);
        naive::NaiveBook wn(kMin, kMax);
        (void)run(we, warm, 2'000, ghz);
        (void)run(wn, warm, 2'000, ghz);
    }

    std::printf("\nENGINE vs NaiveBook — the oracle as a performance baseline (D24)\n");
    std::printf("NaiveBook: std::map per side, vector per level, O(n) cancel scan.\n");
    std::printf("Shares no code with the engine, which is what makes it a fair strawman.\n\n");
    std::printf("%8s | %-27s | %-27s | %s\n", "depth",
                "add, ns/op (eng / naive)", "cancel, ns/op (eng / naive)",
                " cxl ratio [range]");
    std::printf("---------+-----------------------------+"
                "-----------------------------+------------------\n");

    for (const std::size_t depth : {1'000u, 2'000u, 4'000u, 8'000u, 16'000u}) {
        const std::size_t cancels = depth / 2;
        const auto        cmds    = make_stream(4242u, depth, cancels);

        std::vector<double> e_add, e_cxl, n_add, n_cxl, ratios;
        for (int rep = 0; rep < kReps; ++rep) {
            EngineAdapter    fast(1u << 17);
            naive::NaiveBook slow(kMin, kMax);
            const Timing e = run(fast, cmds, depth, ghz);
            const Timing n = run(slow, cmds, depth, ghz);
            e_add.push_back(e.build_ns_per_op);
            n_add.push_back(n.build_ns_per_op);
            e_cxl.push_back(e.cancel_ns_per_op);
            n_cxl.push_back(n.cancel_ns_per_op);
            ratios.push_back(n.cancel_ns_per_op / e.cancel_ns_per_op);
        }

        std::sort(ratios.begin(), ratios.end());
        std::printf("%8zu | %11.1f / %13.1f | %11.1f / %13.1f |  %5.1fx  [%.0f-%.0f]\n",
                    depth, median_of(e_add), median_of(n_add),
                    median_of(e_cxl), median_of(n_cxl),
                    median_of(ratios), ratios.front(), ratios.back());
    }

    std::printf("\nHOW TO READ THIS\n");
    std::printf("  · Every cell is the MEDIAN of %d runs done inside this binary, and\n", kReps);
    std::printf("    the bracket is the full min-max of the ratio across those runs.\n");
    std::printf("    If the bracket is wide, the median is not a quotable figure.\n");
    std::printf("  · The ratio at one depth is a constant factor and proves little.\n");
    std::printf("    A ratio that GROWS with depth is the complexity claim: the naive\n");
    std::printf("    cancel scans O(n) where the id index is O(1), so cxl should climb\n");
    std::printf("    while add stays roughly flat.\n");
    std::printf("  · Not a pinned-core measurement. Same caveat as bench_latency.\n");
    std::printf("  · NaiveBook is deliberately dumb. This is not a claim about\n");
    std::printf("    std::map, it is a claim about THIS design against the obvious one.\n");
    std::printf("  · Engine cancel is NOT flat with depth: it grows sub-linearly,\n");
    std::printf("    because emptying the best level scans the occupancy bitmap at\n");
    std::printf("    O(range/64). The naive scan is the O(n) one.\n\n");
    return 0;
}
