# Matching Engine

A single-symbol, price-time-priority limit order book in **C++20**, built with
`-std=c++23` and using C++23's `std::to_underlying` in the log serialiser.

That phrasing is deliberate. An earlier version of this file said "in C++23",
and the codebase used no C++23 feature at all — the newest thing in it was
designated initializers, which is C++20. `std::expected` is the feature this
design actually wants, and it needs a newer compiler than the build environment
has.

> **Status: every v0.1 scope item is complete and green**, including the profiling
> pass that was the last one outstanding. Amend is cut and the SPSC ring is deferred,
> both deliberately and both recorded. Nothing here is claimed to be finished that is
> not; what is left is a decision about shipping, not about building.

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
| Profiling pass + allocation audit | done |
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

**Four** allocations at construction — the level array, the occupancy bitmap, the
pool's slab, and its free list — plus the id index. After that `Engine::apply`
never calls the allocator, on any path, verified by an instrumented
`operator new` that counts the *aligned* overloads too.

Two allocation sites remain **outside** `Engine` but reachable from `apply`, which
is precisely why they were missed the first time — "outside the class" is not "off
the hot path".

- **The caller's `std::vector<Trade>&`** reallocates if one sweep outgrows it.
  `max_trades_per_apply()` now exposes the bound (`pool capacity + 1`, because every
  trade but the last fully consumes a resting maker), and both benchmarks **verify**
  the vector's capacity never changed rather than assuming it.
- **An attached `EventSink`** allocates per event. `VectorSink` is a test sink,
  attached in no benchmark, and now has `reserve()`. Worth knowing why the obvious
  measurement misses this: `push_back` is amortised O(1), so allocations *per event*
  round to 0.00 while the growth reallocation is O(events so far). Counting
  allocations per operation hides it exactly as it hid the rejected index in D18.

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
Default suite     63 cases, 152,766 assertions          1.3 s
Fuzz gate         1.1M operations, 763,621 assertions   7m01 s
```

Both run under **AddressSanitizer and UndefinedBehaviorSanitizer**.

Four independent mechanisms, because each catches what the others cannot:

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
4. **Conservation, log against book.** Blueprint §4.5's headline property:
   `original == Σ fills + resting remainder + cancelled remainder`. Note that
   folded from the log *alone* this cannot fail — define the remainder as
   `quantity - filled` and it is true by construction. It only has content when the
   resting term comes from an independent source, so this folds the log into a
   quantity ledger and checks it against what the book actually holds: the same
   count of orders, the same ids, the same remaining on each, and the same total
   walked from the book's own lists. A quantity that leaks fails here and nowhere
   else.

Plus deterministic replay: ~10,000 commands through two fresh engines produce
**byte-identical** event logs.

Both property checkers and the shrinker have their own tests — a checker that never
fails proves nothing, so each check is exercised against a deliberately planted
violation. Where a check is sampled rather than run every operation, the frequency is
stated in the test, because a silently sampled check reads as a total one.

## Measurements

> **Not pinned-core numbers, and must not be quoted as such.** Taken under WSL2,
> which is a virtual machine; an isolated core is not achievable from inside a
> guest. p50 through p99.9 sit well below the millisecond spikes a hypervisor
> produces and are believable; **`max` is an upper bound on the environment, not
> on the engine.**

g++ 11.4, `-O2 -DNDEBUG`, no sanitizers, 5 runs × 200k operations, warm.
Per-operation timing via `rdtscp` + `lfence`; TSC rate measured, not assumed;
timer floor ~10 ns. Percentiles are computed per run then medianed.

**p99.9 first, deliberately** — this is a latency system, and the mean of a
latency distribution is a number nobody experiences.

| Operation | p99.9 | p99 | max | (p50) |
|---|---|---|---|---|
| all | 434 ns | 178 ns | 24 µs | 42 ns |
| add, rested | 267 ns | 119 ns | 24 µs | 39 ns |
| add, traded | 313 ns | 180 ns | 14 µs | 63 ns |
| cancel, hit | 564 ns | 356 ns | 9 µs | 87 ns |
| cancel, unknown | 461 ns | 169 ns | 14 µs | 35 ns |

Throughput: median 12.9 M ops/sec.

Each figure is already a median across 5 runs, and **consecutive invocations of the
whole benchmark still move by roughly 5%** on p99.9 — a previous invocation gave 455
ns for `all` where this one gives 434. That spread is the environment, not a change
in the code, and quoting a single run to three significant figures would imply a
precision this setup does not have.

The interesting number is `max`. It was ~1 ms before the id index was made
bounded; the unbounded reallocation it removed cost **8.1 ms in a single
operation** at 2M orders. `SYSTEM-DESIGN.md` D18 records that attempt as
rejected, because getting it wrong is the more useful half of the story.

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
disagrees with its own design document — including D18, a change that was made,
measured, found to be worse than what it replaced, and reverted.

An adversarial audit of the whole codebase against its own stated design intent
found 25 issues; the ones that changed code are recorded in D19 through D21. The
recurring failure it identified is worth naming: **a decision's rationale lives
in the phase that made it, and later phases do not re-read it.** The pool was
built to keep the allocator off the hot path, and three phases later an index was
added that put it back.
