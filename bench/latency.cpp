// bench/latency.cpp — per-operation latency of Engine::apply.
//
// Blueprint §8: no claim without a measurement, no measurement without a
// methodology. This program is the methodology, so it reports the environment
// and its own noise floor alongside the numbers, and refuses to run in any
// configuration whose numbers would be meaningless.
//
// Rationale in SYSTEM-DESIGN.md D17.
#include "me/asan.hpp"
#include "me/engine.hpp"
#include "me/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#  define ME_HAS_TSC 1
#else
#  define ME_HAS_TSC 0
#endif
#include <vector>

using namespace me;
using Clock = std::chrono::steady_clock;

namespace {

// What kind of work an operation turned out to be. Blending these into one
// number hides the story: resting is a pointer bump, a deep sweep is a walk.
enum class Kind : std::uint8_t { Rest, Trade, Cancel, Reject, kCount };

const char* name_of(Kind k) {
    switch (k) {
        case Kind::Rest:   return "add, rested";
        case Kind::Trade:  return "add, traded";
        case Kind::Cancel: return "cancel, hit";
        case Kind::Reject: return "cancel, unknown";
        default:           return "?";
    }
}

struct Sample {
    std::uint64_t ns;
    Kind          kind;
};

std::uint64_t percentile(std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

// FINDING that shaped this file: steady_clock on this machine ticks in ~100ns
// steps, and an engine operation is faster than that. Timing one operation with
// it yields 0 or 100 and nothing between — quantisation dressed up as a
// percentile. So the measured path uses the CPU's timestamp counter instead,
// and steady_clock is kept only to calibrate it and to time whole runs.

// Smallest non-zero step steady_clock can express. This is granularity, which
// is a different and more dangerous problem than overhead: overhead shifts a
// number, granularity invents one.
std::uint64_t clock_granularity_ns() {
    std::uint64_t best = ~std::uint64_t{0};
    for (int i = 0; i < 200'000; ++i) {
        const auto a = Clock::now();
        Clock::time_point b;
        do { b = Clock::now(); } while (b == a);
        const auto d = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        if (d > 0) best = std::min(best, d);
    }
    return best;
}

#if ME_HAS_TSC
// rdtscp waits for earlier instructions to retire; the lfence stops later ones
// being hoisted above it. Without both, out-of-order execution moves work
// across the measurement boundary and the sample is of the wrong thing.
inline std::uint64_t tsc_now() {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#else
inline std::uint64_t tsc_now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}
#endif

// Cycles per nanosecond, measured rather than assumed. Nominal clock speed is
// not the TSC rate on every machine, and guessing it would put a systematic
// error into every number the rig reports.
double calibrate_tsc_per_ns() {
    const auto  wall0 = Clock::now();
    const auto  tsc0  = tsc_now();
    while (std::chrono::duration<double>(Clock::now() - wall0).count() < 0.20) { }
    const auto  tsc1  = tsc_now();
    const auto  wall1 = Clock::now();
    const double ns = std::chrono::duration<double, std::nano>(wall1 - wall0).count();
    return static_cast<double>(tsc1 - tsc0) / ns;
}

// The noise floor: an empty measurement. Anything at or below this is the rig
// measuring itself, not the engine.
std::uint64_t measure_timer_floor() {
    constexpr int kReps = 200'000;
    std::vector<std::uint64_t> samples;
    samples.reserve(kReps);
    for (int i = 0; i < kReps; ++i) {
        const auto a = tsc_now();
        const auto b = tsc_now();
        samples.push_back(b - a);
    }
    std::sort(samples.begin(), samples.end());
    return percentile(samples, 0.50);
}

struct Command {
    bool     is_cancel;
    NewOrder order;
    OrderId  cancel_id;
};

// Deterministic workload. Fixed seed so a re-run measures the same work, which
// is the only way a before/after comparison means anything.
std::vector<Command> make_workload(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price_of(9'900, 10'100);
    std::uniform_int_distribution<int> qty_of(1, 500);
    std::uniform_int_distribution<int> roll(0, 99);

    std::vector<Command> cmds;
    cmds.reserve(n);
    OrderId next_id = 1;

    for (std::size_t i = 0; i < n; ++i) {
        const int r = roll(rng);
        if (r < 30 && next_id > 1) {
            // Cancels dominate real message flow (Module 1.5). A mix without
            // them measures a workload no venue has ever seen.
            std::uniform_int_distribution<unsigned long long> pick(1, next_id - 1);
            cmds.push_back(Command{true, NewOrder{}, static_cast<OrderId>(pick(rng))});
        } else {
            cmds.push_back(Command{false,
                NewOrder{
                    .side        = (r % 2 == 0) ? Side::Buy : Side::Sell,
                    .type        = OrderType::Limit,
                    .price       = static_cast<Price>(price_of(rng)),
                    .quantity    = static_cast<Quantity>(qty_of(rng)),
                    .participant = 1,
                },
                0});
            ++next_id;
        }
    }
    return cmds;
}

struct RunResult {
    std::vector<Sample> samples;
    double              ops_per_sec = 0.0;
};

RunResult one_run(const std::vector<Command>& warm, const std::vector<Command>& measured) {
    Engine eng(9'000, 11'000, 1 << 20);
    std::vector<Trade> trades;
    trades.reserve(64);

    // Warm-up: caches, branch predictors, and the pool's pages all need to be
    // touched before the numbers mean steady state rather than first-touch.
    for (const Command& c : warm) {
        trades.clear();
        if (c.is_cancel) eng.apply(Cancel{c.cancel_id});
        else             eng.apply(c.order, trades);
    }

    RunResult out;
    out.samples.reserve(measured.size());   // no allocation inside the timed loop

    const auto wall_start = Clock::now();
    for (const Command& c : measured) {
        trades.clear();
        Kind kind;
        const auto t0 = tsc_now();
        if (c.is_cancel) {
            kind = eng.apply(Cancel{c.cancel_id}) ? Kind::Cancel : Kind::Reject;
        } else {
            eng.apply(c.order, trades);
            kind = trades.empty() ? Kind::Rest : Kind::Trade;
        }
        const auto t1 = tsc_now();
        out.samples.push_back(Sample{t1 - t0, kind});   // CYCLES, converted at report time
    }
    const auto wall = Clock::now() - wall_start;

    const double secs = std::chrono::duration<double>(wall).count();
    out.ops_per_sec = static_cast<double>(measured.size()) / secs;
    return out;
}

} // namespace

int main() {
    // ---- refuse configurations whose numbers would be meaningless ----------
    if (ME_HAS_ASAN) {
        std::fprintf(stderr,
            "REFUSING TO RUN: built with AddressSanitizer.\n"
            "ASan costs roughly 2x. A latency number measured under it is inflated and\n"
            "unreproducible. Build with -DCMAKE_BUILD_TYPE=Bench.\n");
        return 2;
    }
#ifndef NDEBUG
    std::fprintf(stderr,
        "REFUSING TO RUN: assertions are enabled (NDEBUG is not defined).\n"
        "check_invariants-style asserts are on the measured path. Use the Bench config.\n");
    return 2;
#endif

    constexpr std::size_t kWarm     = 200'000;
    constexpr std::size_t kMeasured = 200'000;
    constexpr int         kRuns     = 5;

    std::printf("=== matching-engine latency ===\n\n");
    std::printf("METHODOLOGY\n");
    std::printf("  compiler        %s\n",
#if defined(__clang__)
                "clang " __clang_version__
#elif defined(__GNUC__)
                "g++ " __VERSION__
#else
                "unknown"
#endif
    );
    std::printf("  build           Bench (-O2, NDEBUG, no sanitizers)\n");
    std::printf("  runs            %d, fresh engine each; percentiles computed PER RUN then medianed\n", kRuns);
    std::printf("  warm-up         %zu ops discarded before each measured run\n", kWarm);
    std::printf("  measured        %zu ops per run, timed individually\n", kMeasured);
    std::printf("  workload        deterministic, fixed seed; 30%% cancels; ~200 tick band\n");

    const std::uint64_t granularity = clock_granularity_ns();
    const double        tsc_per_ns  = calibrate_tsc_per_ns();
    const std::uint64_t floor_cyc   = measure_timer_floor();

    std::printf("  timer           %s, per operation\n",
                ME_HAS_TSC ? "rdtscp + lfence (cycles)" : "steady_clock (no TSC on this target)");
    std::printf("  steady_clock    granularity %llu ns -- TOO COARSE for a single operation,\n",
                static_cast<unsigned long long>(granularity));
    std::printf("                  which is why the measured path does not use it\n");
    std::printf("  TSC rate        %.4f cycles/ns (measured, not assumed)\n", tsc_per_ns);
    std::printf("  timer floor     %llu cycles (~%.1f ns) for an empty measurement\n",
                static_cast<unsigned long long>(floor_cyc),
                static_cast<double>(floor_cyc) / tsc_per_ns);

    std::printf("\n  ENVIRONMENT CAVEAT: if this is WSL2 it is a virtual machine, and an\n");
    std::printf("  isolated pinned core is not achievable from inside a guest. Treat p50 as\n");
    std::printf("  meaningful and the far tail as contaminated by the hypervisor. Do NOT\n");
    std::printf("  quote these as pinned-core numbers.\n\n");

    // D21/F13: ONE continuous stream, split into warm and measured.
    //
    // Previously the two were generated independently, each numbering its cancel
    // targets from id 1. By the time the measured stream ran, the engine's
    // next_id_ was already past everything it tried to cancel, so 92% of cancels
    // MISSED and `cancel, hit` was 2.4% of samples — the population the "cancel
    // is 2.4x a rest" conclusion was drawn from. Continuous ids mean the
    // measured phase cancels orders the warm phase actually rested.
    const auto stream   = make_workload(kWarm + kMeasured, 1u);
    const std::vector<Command> warm(stream.begin(), stream.begin() + kWarm);
    const std::vector<Command> measured(stream.begin() + kWarm, stream.end());

    // D21/F12: percentiles are computed PER RUN and then medianed across runs.
    //
    // The previous version pooled all 5 runs and took percentiles of the union,
    // while printing "median of per-run percentiles". Pooling lets whichever
    // single run caught the worst hypervisor jitter dominate the tail; a median
    // across runs is robust to exactly that, which is why the line was written.
    // The file whose stated purpose is "this program is the methodology" was
    // misdescribing its own methodology.
    constexpr int kKinds = static_cast<int>(Kind::kCount);
    std::vector<std::vector<std::uint64_t>> pct_all;          // per run: p50,p90,p99,p999,max
    std::vector<std::vector<std::vector<std::uint64_t>>> pct_kind(kKinds);
    std::vector<std::size_t> n_kind(kKinds, 0);
    std::vector<double>      throughputs;

    auto five = [](std::vector<std::uint64_t>& v) {
        std::sort(v.begin(), v.end());
        return std::vector<std::uint64_t>{percentile(v, 0.50), percentile(v, 0.90),
                                          percentile(v, 0.99), percentile(v, 0.999),
                                          v.empty() ? 0 : v.back()};
    };

    for (int r = 0; r < kRuns; ++r) {
        const RunResult res = one_run(warm, measured);
        throughputs.push_back(res.ops_per_sec);

        std::vector<std::vector<std::uint64_t>> bucket(kKinds);
        std::vector<std::uint64_t> everything;
        everything.reserve(res.samples.size());
        for (const Sample& s : res.samples) {
            bucket[static_cast<std::size_t>(s.kind)].push_back(s.ns);
            everything.push_back(s.ns);
        }
        pct_all.push_back(five(everything));
        for (int k = 0; k < kKinds; ++k) {
            n_kind[static_cast<std::size_t>(k)] += bucket[static_cast<std::size_t>(k)].size();
            pct_kind[static_cast<std::size_t>(k)].push_back(five(bucket[static_cast<std::size_t>(k)]));
        }
    }

    auto median_of = [&](const std::vector<std::vector<std::uint64_t>>& runs, std::size_t which) {
        std::vector<std::uint64_t> v;
        for (const auto& r : runs) v.push_back(r[which]);
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    auto report = [&](const char* label, const std::vector<std::vector<std::uint64_t>>& runs,
                      std::size_t n) {
        if (n == 0) { std::printf("  %-18s (none)\n", label); return; }
        auto ns = [&](std::size_t which) {
            return static_cast<double>(median_of(runs, which)) / tsc_per_ns;
        };
        std::printf("  %-18s n=%-8zu p99.9=%8.0f  p99=%7.0f  max=%9.0f   (p90=%5.0f p50=%5.0f)\n",
                    label, n, ns(3), ns(2), ns(4), ns(1), ns(0));
    };

    // p99.9 FIRST, deliberately. Course 2.1: "the mean of a latency distribution
    // is a number nobody experiences"; the tail is measured at the moment of
    // maximum economic consequence. An earlier version of this report led with
    // p50 and a 47% median improvement, which concealed a 2.8x p99.9 REGRESSION
    // on the dominant operation. Column order is not cosmetic.
    std::printf("LATENCY, NANOSECONDS per operation — median of per-run percentiles\n");
    std::printf("  %-18s %-10s %8s  %7s  %9s   %s\n", "", "", "p99.9", "p99", "max", "(p90 / p50)");
    report("ALL", pct_all, static_cast<std::size_t>(kMeasured) * kRuns);
    for (int i = 0; i < kKinds; ++i) {
        report(name_of(static_cast<Kind>(i)), pct_kind[static_cast<std::size_t>(i)],
               n_kind[static_cast<std::size_t>(i)]);
    }

    std::sort(throughputs.begin(), throughputs.end());
    std::printf("\nTHROUGHPUT\n  median %.2f M ops/sec across %d runs\n",
                throughputs[throughputs.size() / 2] / 1e6, kRuns);

    std::printf("\nREAD THIS BEFORE QUOTING ANY NUMBER ABOVE\n");
    std::printf("  · The timer floor is ~%.1f ns. Nothing at or below that is a measurement\n",
                static_cast<double>(floor_cyc) / tsc_per_ns);
    std::printf("    of the engine; it is the rig measuring itself.\n");
    std::printf("  · These are warm, steady-state numbers. Cold-start is a different question.\n");
    std::printf("  · The tail is environment-dependent. State where it was measured.\n");
    return 0;
}
