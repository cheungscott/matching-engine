# SYSTEM-DESIGN — Matching Engine

Running log of design decisions, alternatives, and reasoning. Per the Blueprint's
advice: when you disagree with the blueprint mid-build, write your reasoning
*here first*, then check back against the answer-key. Sometimes you'll be right.

Full rationale lives in the vault (`Matching-Engine-Design.md` +
`Matching-Engine-Blueprint.md`); this file captures decisions *as you build*.

---

## Decision log

### D0 — Value types: aliases vs strong types (Phase 0)
- **Chosen (for now):** plain aliases — `Price = int64_t`, `Quantity = uint64_t`, etc.
- **Alternative:** wrap `Price` in a `struct` with `operator<=>` so price/quantity
  mixing is a compile error.
- **Why aliases for Phase 0:** lowest friction to get the compile/test loop green;
  scoped enums (`Side`, `OrderType`) already make the highest-frequency misuse
  (wrong enum) a compile error. Blueprint §3.5 calls aliases a defensible start.
- **Revisit trigger:** the first time a price/quantity swap slips through — upgrade
  `Price` to a strong type early in Phase 1.

### D1 — Integer tick prices, never float (Phase 0)
- **Chosen:** prices are integer ticks.
- **Why:** binary float can't represent 10.1 + 0.2 exactly; float price-level keys
  would corrupt matching. Tick cash-value is out of scope for the engine.

### D2 — `participant_id` present from Phase 0
- **Chosen:** carry `ParticipantId` on `Order` from day one.
- **Why:** self-trade prevention (v1.5, Blueprint §4.7) becomes a logic change, not
  a schema migration, if the field already exists. Fields are cheap; migrations aren't.

### D3 — Toolchain: std::print unavailable on MinGW g++ 15.2
- **Found:** `<print>` fails to link on this build (`std::__open_terminal` undefined).
- **Chosen:** use `std::format` + `std::cout` / fmtlib instead of `std::print`.

### D4 - Sanitizers tied to build type, not defaulted ON (Phase 1)
- **Was:** `option(ME_ASAN ... ON)` applied ASan+UBSan to *every* build.
- **Chosen:** three build types. `Debug` (-O0 -g, ASan+UBSan) for the whole test
  suite, `TSan` (-O1 -g, TSan) for the Phase 8-9 ring only, `Bench` (-O2, no
  sanitizers) as the ONLY build allowed to produce a latency number.
- **Why:** ASan costs ~2x. A Phase 10 p99 measured under it is inflated and
  unreproducible, which voids the §8 measurement discipline and would have put a
  wrong number on the CV. ASan and TSan cannot be enabled together, hence three.
- **Also:** CMake now warns loudly on a Debug build under MinGW, which silently
  has no sanitizer at all.

### D5 - ObjectPool must poison manually - OPEN, decide before writing it
- **Finding:** ASan does **not** catch use-after-release in a slab pool by
  default. The slab is one live allocation forever, so `release()` is invisible
  to it. Verified: the identical bug fires on `new`/`delete` and is silent on a
  pool. Blueprint invariant 7 therefore is NOT self-enforcing.
- **Required:** `ASAN_POISON/UNPOISON` on release/acquire, via `me/asan.hpp`.
- **Two constraints, both verified experimentally:**
  1. Poison whole **aligned 8-byte granules**. Shadow granularity is 8 bytes, so
     a sub-granule poison is silently ignored - a 4-byte poison did nothing.
  2. Keep the **free-list link outside the poisoned region**, or walking your own
     free-list trips the sanitizer.
- **Still yours to decide:** where the link lives, hence `Order` field order;
  fixed capacity vs grow on exhaustion; behaviour when `acquire()` finds it empty.

**Update (D7 resolves the blocking part).** With links at offsets 48-63, BOTH
free-list designs now have a valid, 8-aligned poison span, so D7 does not force
this choice:

| Free-list design | Poison span on release | Coverage |
|---|---|---|
| separate index array | `[0, 64)` - the whole object | links poisoned too, so walking a released order's `next` is caught |
| reuse `Order::next` | `[0, 48)` | links must stay readable, so that bug is NOT caught |

**Recommendation (final call belongs to whoever writes the pool):** the separate
index array. It costs 4 bytes per slot, it poisons strictly more, and it keeps
`ObjectPool<T>` genuinely generic - reusing `next` silently requires every `T` to
have a `next` pointer, which is a hidden coupling. Reusing `next` is a legitimate
memory optimisation later; it is entirely internal to `ObjectPool`, so switching
costs nothing.

### D6 - v0.1 gate re-cut: correctness + measurement, concurrency deferred (2026-08-29)
- **Chosen:** v0.1 shipping on Sun 6 Sep = **Phases 1-7 (correct, oracle-verified,
  fuzz-green book) + the Phase 10 benchmark rig + one honest `perf` pass**.
  Phases 8-9 (SPSC ring, single-writer integration) DEFER to v1.5.
- **Was:** the locked plan's minimum signal-bearing v0.1 = Phases 1-4 + 8-9, i.e.
  the book plus the ring, with metrics following separately Sep 7-18.
- **Why the swap.** Under the 2026-08-19 positioning pivot the engine's job is to
  win the Jan-Apr 2027 host-matching call, not to sit on the CV as a title. In that
  room "I built a matching engine" is unremarkable; "I profiled it, `perf` fingered
  X, I changed Y, p99 went A to B" is not. A ring buffer with no measurement reads
  as copied; a single-threaded book with real profiling data reads as an engineer.
  Also: the TARGET CV bullet is *gated* on a measured number, so shipping without
  Phase 10 means the CV gains nothing at all on 6 Sep.
- **Cost accepted:** the repo loses its most scannable "wants systems" keyword
  (lock-free / single-writer) until v1.5. Mitigated by the README stating the ring
  as designed-and-deferred with the Blueprint §5.2 reasoning intact.
- **Cheap win folded in:** point the Phase 10 rig at the `NaiveBook` oracle as a
  performance baseline, not just a correctness oracle. That converts the tick-array
  and object-pool choices (Blueprint §3.2, §3.3) from asserted to measured for
  near-zero extra build cost, and supplies the before/after delta the profiling
  story needs.
- **Ordering consequence:** Phase 7 (property + differential fuzz) stays the gate,
  but it now gates *measurement* rather than concurrency.
- **Revisit trigger:** if Phases 1-4 are green before Wed 2 Sep, Phase 8 (ring in
  isolation, TSan) re-enters scope - it is self-contained and does not touch the
  book.

### D7 - Order layout: one cache line, aligned, Blueprint field order (Phase 1)

- **Chosen:** `Price` -> `int32_t`, `ParticipantId` -> `uint32_t`, `SeqNum` stays
  `uint64_t`, `alignas(64)` on `Order`, Blueprint §3.5 field order kept, and two
  `static_assert`s pinning both `sizeof` and `alignof` to 64.
- **Result, verified:** `sizeof(Order)==64`, `alignof(Order)==64`, `price@12`,
  `participant@40`, `prev@48`, `next@56`.

**Alternatives, measured rather than argued** (vector of 4-8, counting how many
elements span two 64-byte cache lines):

| Candidate | sizeof | alignof | straddles a line |
|---|---|---|---|
| links last, all 64-bit | 72 | 8 | 4 of 4 |
| links first, all 64-bit | 72 | 8 | 4 of 4 |
| narrowed, no alignas | 56 | 8 | **3 of 4** |
| narrowed + alignas(64) | 64 | 64 | 0 of 4 |
| **chosen: Blueprint order, narrowed, alignas(64)** | **64** | **64** | **0 of 8** |

> [!important] The finding that decided it
> **Fitting inside 64 bytes is NOT sufficient.** Cache lines sit at fixed
> boundaries, so 56-byte objects packed end to end start at 0, 56, 112, 168 and
> only the first begins on a boundary. The 56-byte candidate fits comfortably and
> still straddled 3 times in 4. `alignas(64)` is what buys the guarantee, and it
> is doing real work here, not decoration.

- **Why it matters:** `Order` is the most-touched object in the engine - every
  message reads at least one. A straddling Order costs two memory fetches per
  touch instead of one, permanently.
- **Why narrowing is nearly free:** `side` and `type` are one byte each and
  adjacent, leaving 2 bytes of padding. A 32-bit `price` drops straight into that
  gap. The only genuine cost is the stated range assumption.
- **Assumptions now on the record:** prices fit in ±2.1e9 ticks; fewer than 4.3e9
  distinct participants. Both are far outside any real instrument.
- **What was protected and why:** `SeqNum` stays 64-bit. `entry_seq` IS time
  priority; a wrapped sequence number would let two orders claim the same queue
  position. That is a correctness bug, not a performance one, so it is not a
  field to shrink for convenience.
- **Cost accepted:** 8 bytes of padding per order (56 -> 64). At a 100k-order pool
  that is ~800 KB. Also `Order` is now over-aligned, so any future field that
  pushes it past 64 jumps it to 128 - which is exactly what the size
  `static_assert` exists to catch loudly.
- **Deliberately NOT done:** reordering fields by hot/cold access pattern
  (Blueprint §8). While the whole object is one line the ordering cannot matter,
  because the fetch brings all 64 bytes regardless. It only becomes real if
  `Order` ever exceeds one line.
- **Revisit trigger (Phase 10, measurable):** tight packing without alignment
  fits more orders per line, which favours sweeping a level front to back;
  alignment favours touching one order at a random position. This engine mostly
  does the latter, so alignment should win - but "should" is an argument, not a
  measurement. Good candidate for the Module 4 before/after.

### D8 - Pool safety checks are unconditional; acquire() clears the slot (Phase 1)
**, 2026-08-30.** Starts from PR #1 and fixes what an
independent review found. Recorded here because Scott did not type it.

**The defect.** PR #1 implemented both of `release()`'s safety checks as `assert`. The `Bench`
configuration is `-O2 -g -DNDEBUG`, i.e. the only build allowed to produce a latency number.
`NDEBUG` deletes both. Verified with those exact flags, on the pre-fix code:

- **Double release:** the second call succeeded, `in_use_` underflowed to `SIZE_MAX`, and the
  next two `acquire()` calls returned **the same slot** - verbatim the corruption decision (e)
  claimed to prevent.
- **Foreign pointer:** `next_free_[kNil]` is `vector::operator[]`, unchecked - a wild WRITE
  ~16 GB out. And `poison_payload` ran *before* the check, so 64 bytes the pool does not own
  were already marked dead.
- **Interior pointer:** the range test accepted a pointer 8 bytes into a valid slot, and
  `slot - base` on a non-boundary pointer is UB. g++ 11 produced garbage that was neither in
  range nor `kNil`, so the sentinel check did not fire and release SEGVd - in a **Debug** build
  with ASan, UBSan and asserts all enabled. UBSan did not diagnose the subtraction.

**The rule adopted:** a check that prevents MEMORY CORRUPTION is unconditional; a check that
merely catches programmer error may be `assert`. Both checks here are the former, and both cost
one comparison against a value already in cache.

**And no `assert(false)` on the rejection paths.** That would have re-created the original hole
from the other side: unconditional checks whose failure modes still abort, hence still untestable
under Catch2, which has no death tests. That is exactly why PR #1's two headline properties had
no test and why deleting the poisoning left all 28 tests green. So `release()` returns `bool` and
the pool REPORTS while the caller DECIDES - a `false` at a call site is a caller bug, and the
Engine should assert there, where there is context to say something useful.

**Also fixed:** `index_of` now uses `uintptr_t` (relational comparison of pointers into different
objects is unspecified per `[expr.rel]`) and checks element alignment, not just range. The
constructor's capacity guard moved into the member-init list, where it runs *before* the vectors
are sized - in the body it was dead code, since `bad_alloc` always fired first. `empty()` renamed
to `exhausted()`: it meant the opposite of `PriceLevel::empty()` in the same directory.
`free_list_is_consistent()` added, because `in_use_` and `free_head_` were two sources of truth
that nothing reconciled.

**acquire() now value-initialises the slot.** PR #1 left the previous occupant's bytes, which was
documented and harmless - until `types.hpp` gained intrusive `prev`/`next` (D7). A `push_back`
that reads a stale `next` splices a cycle into a `PriceLevel`, and **ASan cannot see it**, because
those bytes are legitimately live after `acquire()`. Cost is one 64-byte store to a line the
caller is about to write anyway. Reversible if Phase 10 measures it as material.

**Tests added** for every behaviour that had none: double release, foreign pointer, interior
pointer, inherited-state clearing, exhaustion and recovery, free-list consistency under 200
rounds of churn, capacity 0 and 1. 13 pool cases, 3863 assertions, green under ASan.

**Still open, not fixed here:** D5's framing presents the free-list-link choice as two-way, but
`asan.hpp` rule 2 offers a third option that was dropped in every restatement - "keep the link
outside the poisoned region **or unpoison before traversing**". Link-inside-T plus an 8-byte
unpoison costs no second array and constrains `Order` only to "reserve 8 bytes somewhere". It may
still lose on the merits; it was never weighed. Also: PR #1's body and D5 both cite verification
on **g++ 13.3**, which does not exist in this environment (WSL has 11.4 only).

### D9 - PriceLevel: intrusive FIFO, one audited removal path (Phase 1)
**, 2026-09-01.**

- **Chosen:** intrusive doubly-linked list, `prev`/`next` inside `Order`, plus a cached
  `total_quantity_`. One order is one object at one address, so holding an `Order*` IS holding its
  queue position - which is what makes O(1) cancel possible from Phase 4.
- **`unlink()` is THE single removal path** (Blueprint §3.3). Every caller that takes an order out
  of a level goes through it: fill-to-zero, cancel, amend, STP. Never inlined anywhere.
- **Four cases, two branches.** Head / tail / both / neither are the 2x2 combination of two
  independent questions - does `o` have a predecessor, does it have a successor. Writing four
  explicit cases would be four places to get wrong instead of two.
- **`unlink()` nulls `o->prev`/`o->next` on the way out.** Turns a use-after-unlink into an
  immediate null dereference rather than a silent walk into a list the order has left.
- **`total_quantity_` is cached, never recomputed.** The walk is a pointer chase and this is read
  on every incoming order. Invariant 4: it equals the sum of `remaining`.

> [!warning] Ordering precondition, and it is silent if broken
> `unlink()` subtracts `o->remaining`, so **the caller must not zero `remaining` first.** Fill an
> order, set `remaining = 0`, then unlink, and the level subtracts nothing: the cached total stays
> permanently too high with no test failing and nothing for a sanitizer to see. Phase 2 adds
> `reduce_front()` so that quantity changes and list changes each happen in one place.

**Asserts rather than unconditional checks, and why that is consistent with D8.** The rule is
*does removing the check turn a crash into silent corruption?* Without `assert(o != nullptr)` the
next line dereferences null and the process dies at the point of the bug - loud and local.
`assert(total_quantity_ >= o->remaining)` is the honest edge case: under NDEBUG the subtraction
underflows into a huge plausible number. That is data corruption, not memory corruption, so assert
is consistent - but it is a judgement call, and Phase 4's `check_invariants()` is the real net.

**`is_consistent()`** (O(n), tests and Phase 4 only): ends agree, back-links coherent (a list can be
walkable forwards and broken backwards - the classic intrusive bug), `entry_seq` strictly increasing
head to tail (invariant 5), walked sum equals the cached total (invariant 4).

---

## Open questions (from the Blueprint's critique — decide as you reach them)
- Best-price cursor advance: linear scan vs occupancy-bitmap + `countr_zero`? (§3.2)
- Cancel/replace on amend: keep the old order id or mint a fresh one? (§5.5)
- Object-pool exhaustion policy: reject, or grow? (§10g)
- Client-supplied vs engine-assigned order ids? (§10g)
