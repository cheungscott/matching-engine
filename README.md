# Matching Engine

A single-symbol, price-time-priority limit order book in **C++23**.

> **Status: v0.1 in progress.** The order book, matching, cancel, event log and
> verification are complete and green. One item remains before v0.1 ships: a
> profiling pass over the measured hot path. Nothing here is claimed to be
> finished that is not.

---

## What is built

| Component | State |
|---|---|
| `ObjectPool<Order>` — slab + free list, no allocation on the hot path | done |
| `PriceLevel` — intrusive FIFO, O(1) unlink | done |
| `OrderBook` — tick-indexed levels, BBO cursors, id index | done |
| `Engine` — limit and market orders, partial fills, multi-level sweep, cancel | done |
| Sequenced event log + deterministic replay | done |
| `NaiveBook` differential oracle | done |
| Property tests + fuzz gate | done |
| Benchmark rig | done |
| **Profiling pass** | **remaining** |
| Amend | cut from v0.1, deliberately |
| SPSC ring buffer, single-writer ingress | deferred to v1.5, deliberately |

## Architecture

Four components with sharply separated jobs, and the separation is the design:

```
NewOrder / Cancel  →  Engine        POLICY — the only part that decides anything
                        │
                        ├─ OrderBook    STORAGE — price priority. Never decides to match
                        │    └─ PriceLevel   TIME — FIFO by arrival, intrusive links
                        └─ ObjectPool   MEMORY — one slab, allocated once
                        │
                        └────────────→  Event log   the truth; the book is a cache
```

Two dynamic allocations exist in the whole engine, both at construction. After
that the hot path never calls the allocator.

**Why these choices**, in one line each:

- **Tick-indexed array, not `std::map`** — a tree is O(log n) pointer chasing over
  scattered heap nodes; an array index is arithmetic. Costs a bounded tick range,
  which is stated as an assumption.
- **Intrusive doubly-linked list per level** — holding an `Order*` *is* holding its
  queue position, which is what makes cancel O(1) once the index finds it.
- **Pre-allocated pool** — the allocator's worst case is unbounded, and unbounded
  is what disqualifies it, not slow.
- **id → node index** — a cancel message carries only an id. Without the index,
  finding the order is a scan, and cancels are 90%+ of real message traffic.
- **Single-threaded core** — price-time priority requires a total order over
  events, so two threads on one book would serialise anyway. Sequential is the
  honest design here, not a compromise.

## Verification

```
Default suite     60 cases, 151,847 assertions          1.2 s
Fuzz gate         1.1M operations, 763,611 assertions   3m47 s
```

Both run under **AddressSanitizer and UndefinedBehaviorSanitizer**.

Three independent mechanisms, because each catches what the others cannot:

1. **A differential oracle.** `tests/naive_book.hpp` is a deliberately obvious
   `std::map` implementation that shares no code with the engine, and therefore
   cannot share a bug. Identical command streams go to both, and every returned
   id, every trade field, both cursors and the depth at every price are compared
   **after each operation**.
2. **Seven invariants**, checked after every operation in the gate: uncrossed
   book, index coherence, no occupied-but-empty level, level sums, FIFO ordering,
   positive remainders, and pool discipline.
3. **Properties over the event log alone.** A book can satisfy all its own
   invariants and still emit an illegal trade, so these check the *log*: every
   print is at the maker's price, no limit taker does worse than its limit,
   nothing is filled beyond its quantity, a cancelled order never trades again.

Plus deterministic replay: ~10,000 commands through two fresh engines produce
**byte-identical** event logs.

The property checker and the shrinker have their own tests — a checker that
never fails proves nothing.

## Measurements

> **These are not pinned-core numbers and must not be quoted as such.** They were
> taken under WSL2, which is a virtual machine; an isolated core is not
> achievable from inside a guest. `max` reached 1.0 ms, which is the hypervisor,
> not the engine. **p50 is meaningful; the far tail is contaminated.**

g++ 11.4, `-O2 -DNDEBUG`, no sanitizers, 5 runs × 200k operations, warm.
Per-operation timing via `rdtscp` + `lfence`; TSC rate measured, not assumed;
timer floor ~10 ns.

| Operation | p50 | p99 |
|---|---|---|
| add, rested | 73 ns | 187 ns |
| add, traded | 88 ns | 311 ns |
| cancel, hit | 178 ns | 510 ns |
| cancel, unknown | 58 ns | 206 ns |

Throughput: median 9.17 M ops/sec.

The benchmark **refuses to run** if built with a sanitizer or without `NDEBUG`,
so a wrong number cannot be produced by accident.

## Build

Sanitized builds require Linux or WSL — MinGW ships no `libasan`, and TSan has
never been ported to Windows by any vendor.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The fuzz gate is hidden from the default run because it takes ~4 minutes, and a
suite you stop running protects nothing:

```bash
./build/phase1_tests "[gate]"
```

Benchmarks build only under the `Bench` configuration:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Bench
cmake --build build-bench --target bench_latency -j
./build-bench/bench_latency
```

Three configurations, and confusing them is how you get a number you cannot
reproduce: `Debug` carries the sanitizers, `TSan` is for the ring buffer when it
arrives, and `Bench` is the only one permitted to produce a latency figure.

## Layout

```
include/me/types.hpp         Order, Trade, and the value types
include/me/asan.hpp          sanitizer poisoning shim
include/me/object_pool.hpp   slab + free list
include/me/price_level.hpp   intrusive FIFO at one price
include/me/order_book.hpp    tick array, cursors, id index
include/me/engine.hpp        matching policy
include/me/events.hpp        the sequenced event stream
tests/naive_book.hpp         the differential oracle
tests/properties.hpp         properties over the log
tests/scenario.hpp           commands as text, and back
tests/shrink.hpp             failing-stream minimiser
bench/latency.cpp            the benchmark rig
SYSTEM-DESIGN.md             every decision, with the alternatives rejected
```

`SYSTEM-DESIGN.md` is the interesting file. It records what was chosen, what was
rejected, what was measured, and the places where the implementation deliberately
disagrees with its own design document.
