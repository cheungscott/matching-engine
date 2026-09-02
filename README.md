# Matching Engine

A single-symbol **matching engine** in **C++23**: it takes orders, applies
price-time priority, and emits a sequenced event stream.

The limit order book is a *component* of that, not a synonym for it — the storage
layer holding resting orders by price, and within a price by arrival. The
separation is the design, and the diagram below labels it: `Engine` decides,
`OrderBook` stores and never decides to match.

**On the C++23 label**, since it gets abused. The build standard is C++23
(`CMAKE_CXX_STANDARD 23`, `REQUIRED ON`) and `std::to_underlying` is used where log
canonicality depends on an enum's underlying type. Most of the modern facilities
here are C++20: `<bit>`'s `countr_zero`, `countl_zero` and `bit_ceil` do real work
in the cursor advance and in sizing the id index. Absent, with reasons —
`std::expected` (the feature this design actually wants, and the top v1.5 item),
`std::format` and `std::print` all need a newer compiler than the build environment
has, and `std::flat_map` is deliberately unused because reference instability on
insert disqualifies it for a design that stores handles into levels.

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

**Five** allocations at construction — the level array, the occupancy bitmap, the id
index, the pool's slab, and the pool's free list. After that `Engine::apply` never
calls the allocator on any path it owns, verified at 0.00 per operation by an
instrumented `operator new` that counts the *aligned* overloads too.

(The previous version of this line said "four" and then listed five. The version
before *that* said "two". The count is now measured rather than recounted by hand.)

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
- **id → node index**, hashed by scattering 64-slot *blocks* rather than by identity.
  Identity hashing put every live entry in one contiguous run, and linear probing stops
  at the first empty slot, so a lookup landing inside that run walked to the end of it —
  241 µs for a single cancel at 320,000 resting orders, growing linearly with depth
  (D28). A cancel message carries only an id. Without the index,
  finding the order is a scan, and cancels dominate real message traffic. The lookup
  and the unlink are both O(1); a cancel that *empties the best level* additionally
  advances the cursor over the occupancy bitmap at O(range/64), so "O(1) cancel"
  is true of the common path and not of every path.
- **Single-threaded core** — price-time priority requires a total order over
  events, so two threads on one book would serialise anyway. Sequential is the
  honest design here, not a compromise.

## Verification

```
Default suite     88 cases, 152,953 assertions          ~2.7 s
Fuzz gate         1.1M operations, 758,717 assertions   6m22 s
```

Both run under **AddressSanitizer and UndefinedBehaviorSanitizer**.

**These are counts of checks executed. Until recently a large share of them could not
fail** — see below, and D27. The number that matters is not how many assertions run,
it is how many of them constrain the engine.

Four independent mechanisms, because each catches what the others cannot:

1. **A differential oracle.** `tests/naive_book.hpp` is a deliberately obvious
   `std::map` implementation that shares no code with the engine, and therefore
   cannot share a *matching, storage or indexing* bug — it does include the engine's
   header for the shared value types, so "shares no code" would be too strong.
   Identical command streams go to both. Every returned id, every trade field except
   `seq`, and both cursors are compared **after each operation**; the depth at every
   price is compared after each operation in the two small cases, and once at the end
   of the 100k gate case, where a per-operation sweep of the whole tick range would
   dominate the runtime.
2. **Seven invariants**, checked after every operation of the **100,000**-operation
   differential case: uncrossed book, index coherence, no occupied-but-empty level,
   level sums, FIFO ordering, positive remainders, and pool discipline. The separate
   1,000,000-operation case checks log properties only — the two together are the
   "1.1M operations" figure, and they do not both do the same work.
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

### Every checker is itself tested

*"A checker that never fails proves nothing"* was written into this suite early and
applied to exactly one of its checkers. A mutation audit then deleted each clause of
each checker in turn and re-ran everything:

| checker | clauses deleted one at a time | detected |
|---|---|---|
| `props::check` / conservation | 24 branches | 5 |
| `OrderBook::is_consistent` | 9 | **0** |
| `PriceLevel::is_consistent` | 4 | **0** |
| `ObjectPool::free_list_is_consistent` | 2 | **0** |

The million-operation gate — the Blueprint's stated accept criterion — passed an
engine with **time priority destroyed**, because it checked log properties only and no
log property can see the order of a queue. It also passed an engine whose market
takers skipped the best level.

Every branch and every clause now has a planted violation, and each plant asserts
*which* branch fired. That last part is the point: four conservation plants used to
trip the same branch while their comment claimed one per check. The gate now checks
invariants periodically, and a new log property constrains a taker's fill prices.

Where a check is sampled rather than run every operation, the frequency is stated in
the test, because a silently sampled check reads as a total one.

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

| Operation | p99.9 | p99 | (p50) |
|---|---|---|---|
| all | 436 ns | 183 ns | 47 ns |
| add, rested | 350 ns | 130 ns | 41 ns |
| add, traded | 362 ns | 181 ns | 63 ns |
| cancel, hit | 558 ns | 285 ns | 110 ns |
| cancel, unknown | 452 ns | 141 ns | 50 ns |

Throughput: **26 M ops/sec** (20-27 across three invocations).

The `max` column is gone on purpose. It was a *median of five per-run maxima*, which is
not a maximum of anything and quietly discards the worst observation. The binary still
prints it, labelled `medmax`, and it ranges 9-69 µs across invocations — which is the
hypervisor, and is why it is not quoted here.

**Every figure here has been re-measured twice over, and the earlier ones did not
survive either time.** `cancel, hit` p50 was published as 87 ns, corrected to 168, and
is 120 on a quiet machine. Throughput was published as 12.9 M, corrected to 10.2 M,
and is 24-30 M.

That last one is worth explaining, because it moved in the *flattering* direction and
is still the honest number. The timed loop carries two `rdtscp`+`lfence` pairs per
operation and `lfence` is a serialising instruction, so **roughly half of the old
throughput figure was the instrument measuring itself**. Throughput is now measured on
a separate uninstrumented pass over the same workload; the binary prints both, because
the gap between them is the finding.

Spread across invocations: `all` p99.9 gave 458 / 476 / 478 ns here, but earlier
invocations on a busier machine gave 531 and 574. Quote a range, never a figure.

`SYSTEM-DESIGN.md` D18 records a rejected id-index design whose reallocation cost
**8.1 ms in a single operation** at 2M orders. That number belongs to the *rejected*
experiment, not to the shipped predecessor, and it is kept because getting it wrong is
the more useful half of the story.

### Against the oracle it is verified by

`NaiveBook` is the `std::map`-per-side, O(n)-cancel implementation the engine is
differential-tested against. Because it is known *correct* and shares no code, it
doubles as a performance baseline — which is what turns the design claims below from
arguments into measurements.

| depth | add ns/op (eng / naive) | cancel ns/op (eng / naive) | cancel ratio [min-max] |
|---|---|---|---|
| 1,000 | 23.9 / 98.5 | 44.4 / 932 | 20x [16-23] |
| 4,000 | 16.2 / 53.1 | 44.0 / 1,650 | 38x [8-39] |
| 16,000 | 19.6 / 50.6 | 98.6 / 5,482 | 55x [51-91] |

About a fifth of those cancels miss, and in `NaiveBook` a miss scans both maps in full
while a hit scans half the population on average — so the miss share inflates the
ratio. The binary says so in its own output now.

Each cell is the median of **5 runs performed inside the binary**, and the bracket is
the full min-max of the ratio across them. Look at depth 4,000: a median of 38x over a
range of 8-39x. **That is not a quotable number, and the bracket is there so you can
see that without being told.** An earlier version of this file printed a bolded 92x
from three hand-run invocations and no spread at all.

The claim is **not** "N times faster than `std::map`" — `NaiveBook` is deliberately
dumb and that would be dishonest. The claim is about the *shape*: naive cancel goes
932 → 5,482 ns as depth goes 1k → 16k, roughly tracking depth, which is the O(n) scan.
The engine's goes 44 → 99 ns over the same range — **not flat**, because emptying the
best level advances the cursor over the occupancy bitmap at O(range/64), but growing
far slower than linearly.

Add is a flat ~3-4x — a locality win, not a complexity one. At ~200 price levels a tree
lookup is only about 8 comparisons, so calling it a complexity win would be overreach.

```bash
cmake --build build-bench --target bench_baseline -j && ./build-bench/bench_baseline
```

### What the hardware counters say

`tools/perf-pass.sh` regenerates this. 200,000 resting orders, 200,000 measured
operations, median of 3, with a fill-only baseline subtracted.

| mode | cycles | instructions | IPC | cache misses | branch misses |
|---|---|---|---|---|---|
| rest | 117 | 249 | 2.13 | 3.0 | 0.005 |
| trade | 105 | 239 | 2.28 | 0.5 | 0.009 |
| **cancel** | **722** | **363** | **0.50** | **13.2** | **1.24** |
| cancel, miss | 21 | 60 | 2.82 | 1.2 | ~0 |

**Cancel is memory-bound and nothing else is.** IPC 0.50 against 2.1-2.8 elsewhere, on
*fewer* than 400 instructions — it is not doing more work, it is waiting. Thirteen cache
misses from a pointer chase that is inherent to the design: find the node through the
index, touch its predecessor and successor to unlink it, then the pool's free list, then
possibly the occupancy bitmap. Each is a separate line that nothing warmed. That is the
one place where a v1.5 optimisation has a measured case behind it rather than an
argument, and it is deliberately not being done here.

**Branch prediction is a non-issue** — 0.005 to 0.009 misses per operation on the add and
trade paths. The `<=` versus `<` crossing logic and the side dispatch cost essentially
nothing, so any proposal to make this code branchless should be refused unless it comes
with a counter that contradicts this.

Environment limits, because they bound what can be concluded: `L1-dcache-*` and `LLC-*`
are **not exposed under WSL2**, so `cache-references`/`cache-misses` are the whole cache
picture and the level at which they miss is unknown.

This pass found a defect on its first run — an O(resting-orders) lookup in the id index,
241 µs for a single cancel at 320,000 orders, reachable from the public API. See D28. The
argument that the design was safe had been read by three audits and checked by none of
them; running the code checked it.

The benchmarks **refuse to run** if built with a sanitizer or without `NDEBUG`, and
`bench_latency` and `bench_profile` additionally **abort rather than report** if the
caller's trade vector reallocated inside the measured region. A wrong number should
take effort to produce.

## Build

Sanitized builds require Linux or WSL — MinGW ships no `libasan`, and TSan has
never been ported to Windows by any vendor.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`ctest` runs **everything, including the fuzz gate**, which takes several minutes under
the sanitizers. That is deliberate: the gate used to be invisible to `ctest` because
Catch2 does not enumerate hidden tests, so the documented command gave a green run with
the entire Phase 7 gate omitted. The safe thing should happen unless you opt out.

For the fast edit loop, opt out by label:

```bash
ctest --test-dir build -LE gate
```

The gate can also be run directly:

```bash
./build/phase1_tests "[gate]"
```

Benchmarks build only under the `Bench` configuration:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Bench
cmake --build build-bench --target bench_latency -j
./build-bench/bench_latency
```

Three configurations, and confusing them is how you get a number you cannot reproduce:
`Debug` carries the sanitizers, `TSan` is for the ring buffer when it arrives, and
`Bench` is the only one permitted to produce a latency figure.

`Bench` builds **no tests**. It used to build and register the whole suite, so
`ctest --test-dir build-bench` came back green with no sanitizers and every `assert`
compiled out — verifying far less than an identical-looking `Debug` run, with nothing
saying so. Any other build type is now a hard configure error rather than a build with
no flags at all.

## Layout

```
include/me/types.hpp         Order, Trade, and the value types
include/me/asan.hpp          sanitizer poisoning shim
include/me/object_pool.hpp   slab + free list
include/me/price_level.hpp   intrusive FIFO at one price
include/me/order_book.hpp    tick array, cursors, occupancy bitmap
include/me/id_index.hpp      fixed-capacity open-addressed id -> node map
include/me/engine.hpp        matching policy
include/me/events.hpp        the sequenced event stream
tests/naive_book.hpp         the differential oracle
tests/properties.hpp         properties over the log, and conservation
tests/scenario.hpp           commands as text, and back
tests/shrink.hpp             failing-stream minimiser
tests/phase1_tests.cpp       the suite
bench/latency.cpp            per-operation latency percentiles
bench/profile.cpp            allocation counting, and a target for perf
bench/baseline.cpp           the engine against the oracle
SYSTEM-DESIGN.md             every decision, with the alternatives rejected
```

`SYSTEM-DESIGN.md` is the interesting file. It records what was chosen, what was
rejected, what was measured, and the places where the implementation deliberately
disagrees with its own design document — including D18, a change that was made,
measured, found to be worse than what it replaced, and reverted.

An adversarial audit of the whole codebase against its own stated design intent
found 25 issues, recorded in D19 through D23; a second audit before the v0.1 tag found
nine more, including four regressions caused by the first audit's own fixes, recorded
in D25 and D26. The
recurring failure it identified is worth naming: **a decision's rationale lives
in the phase that made it, and later phases do not re-read it.** The pool was
built to keep the allocator off the hot path, and three phases later an index was
added that put it back.
