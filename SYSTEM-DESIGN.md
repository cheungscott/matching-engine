# SYSTEM-DESIGN — Matching Engine

Running log of design decisions, alternatives, and reasoning. Per the Blueprint's
advice: when you disagree with the blueprint mid-build, write your reasoning
*here first*, then check back against the answer-key. Sometimes you'll be right.

Full rationale lives in the vault (`Matching-Engine-Design.md` +
`Matching-Engine-Blueprint.md`); this file captures decisions *as you build*.

---

## Decision log

### D0 — Value types: aliases vs strong types (Phase 0)
> **AMENDED by D7:** this entry says `Price = int64_t`. It is `int32_t` since D7 narrowed it so
> `Order` fits one cache line. The alias-vs-strong-type reasoning below still stands, and its
> revisit trigger is still open — see D20's C++23 list for `operator<=>` on a strong `Price`.
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
> **AMENDED 2026-09-01:** this entry says the fallback is `std::format` + `std::cout`. Neither is
> used anywhere — `std::format` needs g++ 13 and the build environment is 11.4. Everything that
> prints uses `std::printf`. See D20.
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

> [!done] TRIGGER FIRED AND CLOSED - 2026-09-01
> Phases 1-4 came in green on **Mon 1 Sep**, a day inside the window, so the trigger
> fired. **Scott closed it the same day:** 8-9 stay out until v0.1 has shipped. The
> decision was recorded in `WORKING-RULES.md` and NOT here, which left a reader of this file
> alone looking at an open trigger with no resolution. A decision log that records the
> trigger but not the firing is worse than one that records neither, because it looks
> complete.
>
> **The other half of D6 went unbuilt for longer.** The "cheap win folded in" -
> pointing the Phase 10 rig at `NaiveBook` as a *performance* baseline - was never
> done, while the README went on asserting the two claims D6 promised to measure.
> Built 2026-09-01 as `bench/baseline.cpp`; see **D24**.

### D7 - Order layout: one cache line, aligned, Blueprint field order (Phase 1)
> **CORRECTED 2026-09-01, re-measured with `offsetof`.** Two errors in the prose below.
> (1) *"A 32-bit `price` drops straight into that gap"* is not what happens. `price` is at
> offset **12**, so the 2 padding bytes at 10-11 remain padding; a 4-byte value cannot start
> at offset 10. What it actually does is **share the 8-byte slot spanning bytes 8-15** with
> `side` and `type` rather than claiming its own slot. Same result, different mechanism.
> (2) The `ParticipantId` narrowing **buys zero bytes**. Measured: `int32` price with a
> `uint64` participant is still 64; `int64` price with a `uint32` participant is still 72.
> **`Price` is the only field whose width decides the cache line.** The recorded assumption
> "fewer than 4.3e9 distinct participants" is therefore being paid for nothing. Harmless, but
> it is not load-bearing the way the price range assumption is, and it should not be defended
> as if it were.
>
> Related cost, worth stating together with the benefit: **F5's signed overflow in
> `checked_span` exists only because `Price` is 32-bit.** Narrowing bought one memory fetch
> per order touch and cost one arithmetic hazard at the range boundary. Both are now on the
> record; the trade still favours narrowing.

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
Starts from PR #1 and fixes what an
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

### D10 - OrderBook: one tick-indexed array, sentinel cursors (Phase 1)

- **One `std::vector<PriceLevel>` for BOTH sides**, indexed by `price - min_price`. A level at 101
  may hold bids while 102 holds asks, in the same array.
  **This is safe only because of invariant 1**: the book is never crossed, so no price can hold both
  sides. If a bid and an ask ever shared a price they would land in the same `PriceLevel` and be
  mixed - the locked-book bug. **The storage layout depends on a matching invariant**; worth saying
  out loud, because it looks like a bug until you see the reason.
  *Alternative:* separate `bids_`/`asks_` arrays make side explicit and remove the dependency, at
  double the level storage. Blueprint §3.5 specifies one array; this follows it.
- **Cursor sentinels: `best_bid_ = min_price - 1`, `best_ask_ = max_price + 1`.** Chosen so the
  ordinary comparisons need no special case - any real bid exceeds the empty-bid sentinel, so the
  first bid ever added is accepted by the same `>` that handles every later one.
  **Consequence:** the constructor rejects a window touching `Price`'s limits, since the sentinel
  would be signed overflow, which is UB rather than a wrap you can rely on.
- **Linear scan to advance a cursor.** The occupancy bitmap + `std::countr_zero` (Blueprint §3.2) is
  the graduation and belongs in Phase 10, after a profiler asks for it. Know both, ship the simple one.
  *Subtlety:* the scan's `&&` short-circuit is load-bearing - `best_bid_ >= min_price_` must be
  tested before `index_of(best_bid_)`, or the scan asserts out of range the moment a side empties.
- **`add()` asserts `!crosses(...)`.** The book is storage, never policy, and this assert is the one
  place that separation is *enforced* rather than described. If it fires, the bug is in the match loop.
- **`crosses()` lives in the book, not the engine**, so the engine's decision and `add()`'s
  precondition consult the same function and cannot disagree. This is the `>=` / `<=` comparison
  where a strict inequality leaves a locked book (Blueprint §4.6).
- **`best_level()` returns a mutable pointer, deliberately.** Blueprint §3.5 calls it a smell to
  accept: the engine drives the fill loop, so matching policy stays out of the book. The purer
  `book.fill_at_best()` hides more but drags policy inwards.

**`is_consistent()`** (O(range)): invariant 1 (uncrossed), invariant 3 (a live cursor points at a
non-empty level), nothing rests strictly inside the spread, and every level's own `is_consistent()`.

### D11 - Engine: match first, rest second (Phase 1)

- **`apply()` is validate, assign identity, then match-or-rest.** Never both in Phase 1: an exact
  full fill consumes the incoming order entirely.
- **Identity is assigned BEFORE matching.** A `Trade` carries `taker_id`, but a fully-filled taker
  never becomes an `Order` at all - it never rests and never takes a pool slot. Its id exists only
  on the trade.
- **`kRejected = 0`.** `next_id_` starts at 1, so 0 is never a valid id and carries the second
  meaning for free. Blueprint §5.1 specifies `std::expected<OrderId, RejectReason>`, which is
  better because it names *why*; WSL's g++ 11.4 has no `std::expected`, so this is a deliberate
  Phase 1 stand-in, replaced in Phase 3 when the reject path grows real reasons.
- **`validate()` rejects quantity 0, non-Limit (Market is Phase 3), and out-of-range price.** That
  last one exists *because of* the bounded array (Blueprint §5.1 `PriceOutOfRange`) - the array
  design creates the reject reason.
- **Pool exhaustion returns `kRejected`**, an honest bounded failure rather than an unbounded
  allocation at peak load.

**Two ordering rules inside `fill()`, both silent if broken:**

1. **Unlink before anything zeroes `remaining`** - see D9. `unlink()` subtracts `o->remaining`.
2. **Read everything needed from the maker BEFORE `pool_.release()`.** After release the slot is
   poisoned and the pointer dead; any read is a use-after-poison. `price` is captured into a local
   before the unlink/release sequence, then passed to `on_level_emptied()`.

Sequence: emit trade -> capture price -> unlink -> notify book if level emptied -> release. Each
step depends on the previous not having destroyed what it needs.

**The trade prints at `maker->price`**, not the taker's - the resting order set the terms
(Blueprint §4.2). Backwards here and every downstream P&L number is wrong.

**The Phase 1 scope boundary is an assert**, `maker->remaining == cmd.quantity`. It makes the
boundary loud rather than silently mis-filling. Under NDEBUG a mismatch would emit a trade for the
maker's amount with the difference vanishing - data corruption, not memory corruption, so assert is
consistent with D8. Temporary either way: Phase 2 makes the case legal and the assert goes.

### D12 - Partial fills: reduce_front, and one place per kind of change (Phase 2)

**`PriceLevel::reduce_front(qty)`** fills the head in place and leaves it at the head. It asserts
`qty < head_->remaining`, so a **full** consumption cannot come through here - that path is
`unlink()`. Two mutually exclusive routes, each maintaining the cached total exactly once.

That is what **retires D9's ordering trap**. The caller no longer has any reason to touch
`remaining` directly, so there is no longer an ordering to get wrong.

**Queue-position rules, straight from Module 1:**

- A **partially filled resting** order keeps its position - it did nothing to deserve losing it.
- A **partially filled incoming** order rests its remainder at its own price, at the **back** of
  that level, because there it is a new arrival.

**Sequence numbers.** Each trade takes its own `next_seq_++`, so the event log records fill order
within a level and `trades[0].seq < trades[1].seq` is a testable property. A resting order's
`entry_seq` is the sequence of its *arrival*, which is what keeps FIFO comparisons between orders
meaningful even though trades consume numbers from the same counter.

**Phase 2 boundary, enforced not documented:** `assert(level->price() == first_best)`. The fill loop
is written in its general form, but walking to a second price level is Phase 3 - which also brings
market orders and, importantly, the `NaiveBook` oracle. Blueprint §2's risk ordering is that the
harder matching work should land *with* the differential test, not before it. Phase 3 deletes this
assert.

**Still an assert rather than unconditional**, consistent with D8: under NDEBUG a walk would produce
a wrong fill, which is data corruption rather than memory corruption.

### D13 - Walk levels, market orders, and the NaiveBook oracle (Phase 3)

**The walk** is the Phase 2 assert deleted. The loop was already general: `crosses()` re-evaluates
against the cursor and `on_level_emptied()` advances it, so the sweep across levels falls out with
no new code. That was the point of writing it in general form and gating it with an assert.

**Market orders.** `can_match()` splits the predicate - a limit crosses on price, a market takes
whatever exists at any price. A market order **never rests**, because it wanted immediacy rather
than a queue position, so an unfillable remainder is cancelled. Blueprint §6.1 emits
`OrderCancelled{NoLiquidity}` there; events arrive in Phase 6, so for now the behaviour is right and
the record is missing. `validate()` skips the tick-window check for markets, whose price field is
meaningless.

**`OrderBook::depth_at(Price)`** added: aggregate resting quantity at a price. That is L2 market
data, and it is O(1) precisely because `PriceLevel` caches its total. The oracle needs it to compare
book state rather than only trades.

### The oracle - `tests/naive_book.hpp`

A deliberately obvious `std::map<Price, std::vector<Resting>>` per side. No pool, no cursors, no
intrusive links, no sentinels. Slow, allocation-happy, checkable by eye.

**Its job is to disagree with the real book when the real book is wrong**, so every optimisation
removed from it is a bug it can still catch. Keep it after Phase 3: it is also the performance
baseline for the Module 4 write-up, which converts the tick-array and pool choices from asserted to
measured at near-zero extra cost.

**The differential runner** feeds identical command streams to both and compares after EVERY
operation: returned id, trade count, and each trade's maker, taker, price and quantity; then
`best_bid`, `best_ask`, and depth at all 21 prices; then `book().is_consistent()`. 2000 operations,
about 59,000 assertions, 0.2 seconds.

- **Fixed seed (20260901)**, so a failure is replayable rather than a story about a run that once
  went wrong.
- **Narrow price band (98-104)** against a 90-110 book, so orders actually cross instead of resting
  past each other. A fuzz that never trades tests nothing.
- **8% market orders** - enough to exercise the sweep-and-cancel path without starving the book.
- **`seq` is excluded** from the comparison: it is an event-numbering scheme, not a matching
  outcome, and NaiveBook does not model the engine's counter. Everything constituting *what
  happened* is compared.
- **Pool capacity 8192**, so exhaustion never fires. The engine would return `kRejected` where
  NaiveBook, having no pool, rests the order - a real design difference between the two, not a bug,
  and the fuzz is not the place to explore it.

**What this protects.** Phases 4-7 each add a command type or an optimisation, and every one gets
diffed against an implementation that shares no code and therefore cannot share a bug. That is a
far stronger claim than any hand-written test, which only covers what its author thought of.

### D14 - Cancel by id: the index, and two audited removal paths (Phase 4)

**The gap Blueprint §3.4 names.** A cancel message carries only an **order id**. Without an index,
finding the order means scanning the book - O(total orders) - and the O(1) cancel claim is silently
false. Since cancels are 90%+ of real message traffic, that is the difference between a fast engine
and a slow one.

`std::unordered_map<OrderId, Order*> by_id_` in `OrderBook`.

> [!note] Divergence from Blueprint §3.5, deliberate
> The Blueprint specifies `OrderHandle { Order* node; Price price; Side side; }`, the extra fields
> intended to save a cache miss by reaching the level without touching the node.
> **They buy nothing here.** Every removal dereferences the node anyway - `unlink()` needs
> `prev`/`next`, and the level total needs `remaining` - so the node's cache line is loaded
> regardless. What the extra fields *do* add is duplicated state that can drift from the order it
> describes, which is another thing invariant 2 would have to police.
> Storing `Order*` alone. If Phase 10 measures a case where the level lookup could usefully be
> prefetched ahead of the node, revisit.

### Two audited paths, one per layer

- **`OrderBook::remove(Order*)`** - unlink from the level, erase the index entry, advance the
  cursor if the level emptied. One step, so no caller can do two of the three.
- **`Engine::retire(Order*)`** - `book_.remove()` then `pool_.release()`.

The book does not own the pool, so the audited step splits at the ownership boundary rather than
being one function that reaches across it. Each layer audits its own state.

**The retrofit did not hurt, and that is the D9 discipline paying off.** Blueprint §12 lists
"Phase 4's index retrofit hurting" as one of the things the project exists to teach. The actual
change to the fill path was **one line**: `unlink` + `on_level_emptied` + `release` became
`retire(maker)`. That is only true because `unlink()` was already the single removal path from
Phase 1 rather than surgery inlined into three call sites. The lesson landed by *not* hurting,
which is worth stating in an interview more than a war story would be.

### `check_invariants()` - all seven

`OrderBook::is_consistent()` covers 1-6; `Engine::check_invariants()` adds 7.

The one worth understanding is **invariant 2** (index coherence). Walking every level and checking
`find(o->id) == o` catches an index entry pointing at the wrong order. It does **not** catch a
*stale* entry - an id in the index whose order is in no level at all, i.e. a dangling `Order*`
pointing into a released pool slot. That is caught by the final line, `counted == by_id_.size()`,
and by nothing else in the system.

Invariant 7 splits the same way: `free_list_is_consistent()` checks the free list's structure, and
`pool_.in_use() == book_.resting_count()` checks the accounting. A leaked slot - acquired, dropped,
never released - is invisible to the first and caught by the second.

**Cancel of an unknown id returns `false`, and that is ROUTINE**, not an error (Blueprint §5.4). A
fill and a cancel legitimately race, and the fill can win. Treating it as an error would make a
normal market event look like a defect.

### The oracle now includes cancels

`NaiveBook::cancel()` finds the order by **scanning every price level and every order in it** -
precisely the O(total orders) walk the index exists to avoid. Writing it out is the clearest
possible statement of what the index buys, and it means the reference implementation cannot share
the index's bugs, because it has no index.

3000 operations mixing limits, markets and cancels, with all seven invariants checked after every
one: ~85,000 assertions, zero diffs.

### D15 - Sequenced event log and replay (Phase 6)

**Events** (`include/me/events.hpp`): `OrderAccepted`, `OrderRejected`, `TradeExecuted`,
`OrderCancelled`, as a `std::variant`. `OrderAmended` is absent because Phase 5 is cut.
`RejectReason` and `CancelReason` are enums, so a reject finally says *why* - which is what
Blueprint §5.1 wanted `std::expected` for, arriving here through a different door.

**`validate()` now returns `std::optional<RejectReason>`** rather than a bool. Same check, but the
outcome carries a reason the event can record.

**`EventSink`**, with a null default. The engine publishes if a sink is attached and does nothing
otherwise, which is why all 53 pre-Phase-6 tests were unaffected. `VectorSink` collects in memory.

> [!note] Transitional shape, deliberately
> `apply()` still fills a `std::vector<Trade>&` *and* publishes to the sink. The end state is
> sink-only - the log is the truth - but changing the signature would have rewritten every existing
> test for no behavioural gain during ship week. The trade vector is test ergonomics; the sink is
> the real output. Unify after v0.1.

### Why the log format is what it is

One line per event, fixed field order, **integers only**. That is a determinism requirement, not
cosmetics, because this text is what the replay test diffs byte for byte:

- **No floats.** Their formatting is locale- and implementation-dependent, so the same run could
  produce different text on a different machine.
- **No addresses**, which vary per run under ASLR.
- **No timestamps.** Latency is physical and never deterministic; the moment a clock reaches the log
  the diff becomes noise. `seq` is the only notion of time, and it is a counter.
- **No hash-order iteration.** `by_id_` is an `unordered_map`, and its iteration order is not
  guaranteed stable - nothing in the log may be derived from walking it.

### Scenarios - `tests/scenario.hpp`

The **input** log, as text: one command per line, integers only. Same shape a NASDAQ ITCH or LOBSTER
day reduces to, which is the on-ramp Blueprint §6.2 mentions. Designing for it now cost nothing.

Determinism is the claim that the input log determines the output log, so both need a stable text
form before it can be tested at all.

### What is actually verified

- **~10,000 commands, two fresh engines, byte-identical logs.** The whole claim of the design in
  one assertion.
- **Round-trip:** commands to text to commands produces identical text, and the reconstructed
  commands produce the identical event log.
- **Sequence numbers strictly increase** across ~2,000 mixed operations. `seq` *is* the order, so a
  repeat or a gap would make the log's central promise false.
- **A negative test.** One extra command must change the log, and the old log must remain a prefix
  of the new one. A replay test that passes no matter what is worthless, and append-only is checked
  rather than assumed.

**Trimmed, per the pre-committed ladder:** the existing 53 tests were NOT backfilled as scenario
files. The replay property is proven by generated streams instead, which is the part that carries
the guarantee; converting hand-written tests to data files is presentation, and ship week is not
when to spend a day on it.

**Found while writing this:** the first version of the event test asserted 7 events where the
engine produces 6. The engine was right - accept, accept, trade, cancel, reject, reject - and the
test's arithmetic was wrong.

> **NUMBERS SUPERSEDED (D26).** The suite and gate figures in this entry (60 cases,
> 151,847 assertions, 763,611 gate assertions, 3m47s, 1.2s, ctest 11.6s) were accurate when
> written and were never updated as tests were added. Current figures live in `README.md`
> and are re-measured, not carried forward. The reasoning below is unaffected.

### D16 - Property tests and the oracle fuzz, without RapidCheck (Phase 7)

> [!note] Divergence from Blueprint §11, deliberate
> Phase 7 specifies **RapidCheck**. This uses a seeded generator plus a hand-written shrinker
> instead. Three reasons:
> 1. The Blueprint's own `FetchContent` snippet pins RapidCheck to **`GIT_TAG master`** - a floating
>    tag, which directly contradicts the discipline already applied to Catch2 (pinned at `v3.5.2`
>    precisely because a floating tag makes builds unreproducible). Pinning it to a commit is
>    possible, but it is a new dependency taken on during ship week.
> 2. **What RapidCheck actually buys over a seeded loop is shrinking**, and that is replaced by
>    `tests/shrink.hpp` - a 30-line delete-chunk minimiser, which has its own test.
> 3. The generator already exists. Phase 6's scenario stream is deterministic, replayable, and
>    serialisable to text.
>
> **Revisit trigger:** after v0.1, if the shrinker proves inadequate on a real failure. It only
> deletes commands; it never simplifies one, so a bug needing a *smaller quantity* rather than
> *fewer commands* will not reduce well.

### Properties are checked against the LOG, not the book

`tests/properties.hpp` takes a `std::vector<Event>` and nothing else.

That is the payoff of "the log is the truth". A book can satisfy every one of its own invariants and
still emit an illegal trade - the invariants describe the book's internal consistency, not whether
the market it produced makes sense. Checking the log instead means the properties are independent of
the implementation, and would still hold against a completely different engine.

Four families, all verified in one ordered pass:

- **Legality.** No zero-quantity trade; price inside the tick window; a trade has two distinct
  orders on opposite sides; **the print is exactly at the maker's accepted price**; a limit taker
  never pays above or receives below its own limit; a market order never rested as a maker.
- **Conservation.** No order is ever filled beyond its accepted quantity, checked incrementally so
  the violating trade is identified rather than just the total.
- **Cancelled never matches.** Order matters here: it is a statement about what follows a cancel,
  not about the set of cancelled ids, which is why the check walks the log in sequence.
- **Structural.** No id reused, no zero-quantity order accepted.

### Scale, split by cost

- **1,000,000 operations, properties only.** Cheap because it is one pass over the log with no
  per-operation O(range) walk. This is the Blueprint's 10⁶ accept criterion.
- **100,000 operations, differential + all seven invariants after every operation.** Expensive by
  construction; this is the one that would catch a book which is internally inconsistent in a way
  the log does not reveal.

Two mechanisms, deliberately: the log-based properties catch an engine that lies about what
happened, the invariant walk catches an engine whose internals rot while the log stays plausible.

### The meta-test

**A checker that never fails proves nothing**, and a million-operation run that can only return
`true` is an expensive way of computing `true`. So four violations are planted into a known-good log
- a trade off the maker's price, an over-fill, an unknown maker, and a cancelled order trading
afterwards - and each must be caught. The shrinker gets the same treatment: reduce a 4,000-command
stream and assert the result still reproduces and is an order of magnitude smaller.

### Runtime split - measured, and it forced a change

The gate runs in **3m47s** under ASan. That cannot sit in the suite you run while working: a
test suite you stop running because it is slow protects nothing, so the slowness would have cost
more than the coverage bought.

The three heavy cases are tagged **`[.gate]`**. A Catch2 tag beginning with `.` hides the case from
the default run *and* from `catch_discover_tests`, so `ctest` skips it too.

| Command | Covers | Time |
|---|---|---|
| `./build/phase1_tests` | 60 cases, 151,847 assertions | **1.2s** |
| `ctest --test-dir build` | same, one process per case | 11.6s |
| `./build/phase1_tests "[gate]"` | 1.1M operations, 763,611 assertions | **3m47s** |

**Run the gate before every commit that touches matching logic, and before the ship.** Not on every
save. This is the same reasoning as the Debug/Bench split: the expensive check earns its keep at a
boundary, not in the inner loop.

### D17 - The benchmark rig, and the clock that could not measure it (Phase 10a)

`bench/latency.cpp`, built only under `CMAKE_BUILD_TYPE=Bench`.

> [!warning] The finding that shaped the file
> The first version timed each operation with `std::chrono::steady_clock`. Every result came back a
> multiple of 100ns - p50=100, p90=200, p99=300 - and the measured "clock overhead" was 0.
> **`steady_clock` here has ~99ns granularity, and an engine operation is faster than that.** Timing
> one operation with it yields 0 or 100 and nothing in between: quantisation dressed up as a
> percentile.
>
> Granularity is a more dangerous problem than overhead. **Overhead shifts a number; granularity
> invents one.** The rig reported plausible-looking percentiles that were pure artefact, and it
> would have been entirely possible to put them on a CV.

**The measured path now uses the CPU timestamp counter**, `rdtscp` plus `lfence`. `rdtscp` waits for
earlier instructions to retire and the fence stops later ones being hoisted above it - without both,
out-of-order execution moves work across the measurement boundary and the sample is of the wrong
thing. `steady_clock` is kept only to calibrate the TSC and to time whole runs.

**The TSC rate is measured, not assumed** (2.9040 cycles/ns here). Nominal clock speed is not the
TSC rate on every machine, and guessing would put a systematic error into every number.

### Deviation from Blueprint §8: no HdrHistogram

The Blueprint specifies HdrHistogram. This stores raw cycle counts in a **pre-reserved** vector and
sorts afterwards. HdrHistogram's advantages are bounded memory and no allocation while recording;
for a fixed 200k-sample run a reserved vector also allocates nothing on the recording path, and
sorting gives **exact** percentiles rather than bucketed ones. No dependency, one fewer thing to
pin. Revisit if a run ever needs unbounded duration.

### What the rig refuses to do

Verified, both paths:

- **Built with a sanitizer → exits 2.** ASan costs ~2x; a number measured under it is inflated and
  unreproducible.
- **Built without `NDEBUG` → exits 2.** Assertions on the measured path are not the code that ships.

This is D4's build-type discipline made *unbypassable from the other direction*: the config cannot
produce a wrong number by accident, and neither can a hand-rolled compile.

### Methodology it reports about itself

Compiler, build type, run count, warm-up size, workload shape, `steady_clock` granularity, measured
TSC rate, and the **timer floor** - the median of an empty measurement, ~29 cycles (~10ns) here.
Anything at or below the floor is the rig measuring itself. Stating it is what lets a reader tell a
real 74ns from noise.

Results are broken out **by operation kind** rather than blended, because resting is a pointer bump
and a sweep is a walk, and one number hides which is which.

### First numbers - NOT for the CV yet

WSL2, g++ 11.4, `-O2 -DNDEBUG`, 5 runs x 200k measured operations, warm.

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| all | 74 ns | 274 ns | 524 ns |
| add, rested | 73 ns | 187 ns | 456 ns |
| add, traded | 88 ns | 311 ns | 559 ns |
| **cancel, hit** | **178 ns** | **510 ns** | 779 ns |
| cancel, unknown | 58 ns | 206 ns | 447 ns |

Throughput: median 9.17 M ops/sec.

**Caveats that travel with these numbers:** WSL2 is a VM, an isolated pinned core is not achievable
from inside a guest, and `max` reached 1.0 ms - which is the hypervisor, not the engine. p50 is
meaningful; the far tail is contaminated. Do not quote these as pinned-core figures.

**The Phase 10b lead is already visible:** `cancel, hit` is 2.4x the cost of resting an order, and
cancels are the message type that dominates real flow. That is where the profiler should be pointed
first - and it is a hypothesis to test, not a conclusion.

### D18 - REJECTED: dense vector id index (Phase 10b, first attempt)
**Recorded because it was WRONG.**

Three source files cited "D18" for a day while this entry did not exist. That is the exact failure
this project is supposed to prevent, in its purest form: the rationale did not merely live in
another phase, it was never written.

**What it was.** Replace `std::unordered_map<OrderId, Order*>` with `std::vector<Order*>` indexed by
id, growing via `resize()`. Motivated by a real, measured finding: the map allocated **0.95 `new`
per resting order and 1.00 `delete` per cancel**, putting the allocator back on the exact path
`ObjectPool` exists to keep it off.

**Why it was rejected.** It replaced *many bounded* allocations with *rare unbounded* ones. Measured
cost of a single `Engine::apply` at the moment of reallocation:

| resting orders | that one call |
|---|---|
| 65,535 | 229 µs |
| 1,048,575 | 4,055 µs |
| 2,097,151 | **8,096 µs** |

The pool's founding rule is *"the allocator's worst case is unbounded, and **unbounded is what
disqualifies it, not slow**."* The replacement satisfied that criterion **less well than the thing
it replaced**. A map node is ~50-100 ns every time; a vector regrow is O(ids ever issued) and ids
never stop.

**And it was reported wrong, twice.** First as a 47% p50 improvement, with a 2.8x p99.9 regression
on `add, rested` (456 -> 1,283 ns) in a footnote. Then `reserve()` was added and reported as the
fix - but the benchmark reserves 4x a 2^20 pool and runs 400k operations, so **growth never fires
in the measurement**. The absence of the defect in a workload that cannot exhibit it was reported
as its absence.

> [!warning] The lesson, which is the point of keeping this entry
> Blueprint §8.3 already listed the correct answer - *"reserve the id index"* - and more
> importantly, the bound was available all along: **the pool caps how many orders can rest at
> once**, so live index entries were never unbounded even though ids are. The fix needed a
> different container, not a bigger guess. See D19.

### D19 - The bounded id index, and the correctness fixes the audit surfaced (Phase 10b)
after an adversarial audit found 25 issues,
of which these are the ones that changed code.

#### `IdIndex` - fixed-capacity open addressing, allocates exactly once

`include/me/id_index.hpp`. Sized from **pool capacity** at construction and never grows, because at
most `capacity` orders can rest at once - the pool says so. Live entries are bounded even though ids
are not, which is the observation both previous attempts missed.

| | allocations | worst single op |
|---|---|---|
| `unordered_map` | 1 per insert, 1 per erase | bounded (~50-100 ns) |
| `vector` by id | rare | **unbounded — 8.1 ms measured** |
| **`IdIndex`** | **zero after construction** | **bounded** |

- **Table size** `bit_ceil(capacity * 2)`, so load factor can never exceed 0.5 and linear probing
  stays short. Asserted, not assumed.
- **Identity hash, deliberately.** Ids are engine-assigned and strictly increasing, so masking
  distributes them perfectly AND keeps recently-issued ids - the ones most likely to be cancelled -
  adjacent in memory. A scrambling hash distributes equally well and destroys that locality. This
  is only safe because ids are never client-supplied; a client-chosen id would make it adversarial.
- **`id == 0` means empty.** `Engine::kRejected` already reserves 0, so the sentinel costs no
  occupancy bit.
- **Backward-shift deletion, not tombstones.** Tombstones accumulate and degrade every later probe;
  in a process that runs all day, a tombstone table gets permanently slower. Backward shift keeps
  the table clean by relocating any element whose probe chain the hole would break.

**Verified:** `bench_profile` reports **0.00 new and 0.00 delete per operation** across rest, trade
and cancel - and for the first time that number is trustworthy, because the counter now overrides
the *aligned* `operator new` too. `Order` is `alignas(64)`, so every `Order` allocation had been
invisible to the instrument built to police allocation (F3).

#### F4 - the log could contradict the book, and a replayer could not repair it

`OrderAccepted` was emitted **before** the pool was consulted. On exhaustion the log read
Accepted-then-Rejected, and `OrderRejected` carries **no id**, so a consumer folding the log rests
an order the book never held. *"Book state == fold(events)"* was false on a reachable path.

Compounding it: D13 deliberately sized the fuzz so exhaustion never fires, and `properties.hpp`
skips `OrderRejected` entirely. **The one path where the log lied was the one path excluded from
every check.**

**Fix:** a limit order reserves its pool slot *before* it is accepted. An order is now only accepted
if the engine can honour it, and a rejected order burns no id. Markets need no slot since they never
rest. Cost is one acquire/release pair on the fully-filled path - both pointer bumps.

#### F5 - signed overflow in `checked_span`, UBSan-confirmed

D10 reasoned about the *sentinels* overflowing and missed the *span*: `max_price - min_price` is
int32 arithmetic. `OrderBook(-2e9, 2e9)` triggered UB before the guard could run. Now widened to
`int64` first, with an explicit level-count bound.

#### F6 - `retire()` discarded `release()`'s report

D8's whole architecture is *"the pool REPORTS and the caller DECIDES"*, and the deciding call site
threw the answer away. The entire return-`bool` design terminated in a shrug. Now asserted.

#### F7 - D8's rule was applied in `ObjectPool` and nowhere else

*"A check preventing MEMORY CORRUPTION is unconditional"* - but `OrderBook::add` and `remove`
guarded unchecked `operator[]` writes with `assert` alone, which `-DNDEBUG` deletes in the Bench
config. `add()` now throws on an out-of-range price; `remove()` returns `false` rather than
corrupting, following the pool's report-don't-abort shape.

#### F25 - market orders put junk in the log

A market order's `price` was copied verbatim into `OrderAccepted`. Two behaviourally identical
market orders carrying different junk produced **different byte-for-byte logs** - a canonicality
hole in the exact artefact the replay test diffs. Now normalised to 0 before it reaches the log.

### D20 - Occupancy bitmap, and what "C++23" actually means here (Phase 10b)

#### The cursor scan was a tail spike, and the profiler finally asked

D10 shipped a linear cursor scan and said the bitmap graduation *"belongs in Phase 10, after a
profiler asks for it."* The audit measured **1,064 scan iterations inside a single operation** on
this project's own benchmark workload, and an adversarial case at 6,625 cycles - 21x a normal
cancel. It asked.

`std::vector<std::uint64_t> occupied_`, one bit per level. `std::countr_zero` / `std::countl_zero`
find the next occupied level in one instruction, so 64 empty levels are skipped per word and the
worst case drops from O(range) to O(range/64).

`is_consistent()` gained a check that the bitmap agrees with the levels: a **stale bit** sends a
cursor to an empty level, a **missing bit** makes a live level invisible, and neither is caught
anywhere else.

#### The honest position on "C++23"

The README claimed C++23 and the codebase used **no C++23 feature at all** - the newest thing in it
was designated initializers, which is C++20. That is a flag, not a language claim, and it would have
been the first line of a CV bullet.

Now used, and used because they earn their place rather than to justify the label:

| Feature | Where | Why it earns its place |
|---|---|---|
| `std::countr_zero` / `countl_zero` (`<bit>`) | cursor advance | the fix for a **measured** tail spike |
| `std::bit_ceil` (`<bit>`) | `IdIndex` table sizing | power-of-two masking, no division |
| `std::to_underlying` (C++23) | `to_line` | cannot pick the wrong type if an enum's underlying type changes - and log canonicality is load-bearing |

Still absent, with reasons:

- **`std::expected`** - Blueprint §7 calls it the flagship. Needs g++ 12; WSL has 11.4. `g++-12` is
  one apt install away and this is the top v1.5 item, since it is the right shape for the reject
  path F4 exposed.
- **`std::format` / `std::print`** - g++ 13 and 14 respectively. Not available.
- **`operator<=>` on a strong `Price`** - D0's revisit trigger. Genuinely valuable (it would make
  price/quantity mixing a compile error) but it touches every file; v1.5.
- **`std::flat_map`** - Blueprint marks it KNOW, DON'T USE. Reference instability on insert
  disqualifies it for a design that stores handles into levels. Deliberately unused.

### D21 - The measurement rig was measuring the wrong thing (Phase 10b)
> **NUMBERS SUPERSEDED (D26).** The percentiles and the 12.2 M ops/sec below did not reproduce on
> re-measurement: `cancel, hit` p50 was quoted at 87 ns and is 168; throughput is 10.2 M. They came
> from single invocations recorded without a second run. Current figures live in `README.md` and
> carry their spread. The methodology argument in this entry - per-run percentiles, p99.9 first -
> is unaffected and is what made the error visible.

Two defects in `bench/latency.cpp`, both found by audit rather than by use, and both of the kind
that produce confident wrong numbers rather than obvious failures.

**F12 - it misdescribed its own methodology.** It printed *"median of per-run percentiles"* and
computed percentiles over all 5 runs **pooled**. Pooling lets whichever single run caught the worst
hypervisor jitter dominate the tail; a median across runs is robust to exactly that, which is
presumably why the line was written that way. The file whose stated purpose is *"this program is
the methodology"* was lying about its methodology. Now it does what it says.

**F13 - the headline finding rested on an artefact.** Warm and measured workloads were generated
independently, each numbering cancel targets from id 1. By the time the measured stream ran, the
engine's `next_id_` was past everything it tried to cancel, so **92% of cancels missed** and
`cancel, hit` was 2.4% of samples. The Phase 10a conclusion *"cancel is 2.4x the cost of resting"*
was drawn from that population. Now one continuous stream is generated and split, so measured
cancels target orders the warm phase actually rested. Hit count roughly doubled.

**And the report now leads with p99.9.** Column order is not cosmetic. The previous layout led with
p50, and that is how a 47% median improvement got reported while a 2.8x p99.9 regression sat in a
footnote. Course 2.1: *"the mean of a latency distribution is a number nobody experiences."*

#### Results, on the metric that matters

Median of per-run percentiles, ns. Original = `unordered_map`; rejected = D18's vector; current =
`IdIndex` + bitmap.

| p99.9 | original | rejected (D18) | **current** |
|---|---|---|---|
| ALL | 524 | 932 | **455** |
| add, rested | 456 | 1,056 | **312** |
| add, traded | 559 | 409 | **367** |
| cancel, hit | 779 | 582 | **663** |
| **max observed** | ~1,000,000 | 1,197,334 | **28,464** |

The max is the number worth looking at: **35x lower**, because the unbounded allocations are gone.
Throughput 12.2 M ops/sec, p50 42 ns - reported last, on purpose.

### D22 - Conservation, and why the Blueprint's own statement of it is vacuous (Phase 7)

Blueprint §4.5 calls conservation *"your single best property test"* and states it as

> `taker.original == Σ fills + rested remainder + cancelled remainder`

with §9.2 asking for it *"per order + global"*. It was the last unimplemented headline property.
`properties.hpp` had a section **labelled** conservation, but it only checked the *inequality*
`filled <= quantity`. The equation was never checked.

#### The finding that shaped the implementation

**Folded from the log alone, that equation cannot fail.** Define the rested remainder as
`quantity - filled` and it is true by construction, whatever the engine did. Implemented the
obvious way it would be an expensive way of computing `true` - the same defect the Phase 7 tests
already guard against by planting violations.

It acquires content only when the resting term comes from an **independent source**. That source
is the book. So what is actually checked is the log's account of what should still be resting
against what the book really holds:

| check | what it proves |
|---|---|
| `book.resting_count() == live.size()` | the two agree on HOW MANY orders rest |
| every live id found, `remaining` matches | the two agree on WHICH, and on each quantity |
| `book.total_resting_quantity() == Σ live` | the book's own walk agrees with its index |
| `accepted == filled + withdrawn + resting` | §4.5, globally, with the book supplying the last term |

The first two prove the **sets** agree; the third and fourth prove the **quantities** do. A
quantity that leaks - a level reduced with no trade emitted, or a trade emitted without reducing
the book - fails here and nowhere else.

`fold_ledger` also asserts its own arithmetic balances. That check **is** forced by the fold, and
the comment says so rather than dressing it up as an engine check: it guards a future edit to
`fold_ledger`, nothing more.

#### `total_resting_quantity()` walks the lists, not the cached totals

`PriceLevel` caches a running `total_quantity_`. Summing those caches would be cheaper, and
useless: `is_consistent()` already validates that cache against the list, so conservation would be
agreeing with a number that another check had already blessed. Walking the intrusive lists
directly makes the two checks independent, which is the entire point of having both. Cost is
O(resting), which is why it is a checkpoint check.

#### Where it runs, stated rather than sampled silently

| site | frequency | why |
|---|---|---|
| `conservation_holds_after_every_operation` | after **every** op, 900 ops | Blueprint §9.2's literal ask |
| differential gate | every 5,000 ops + at the end, 100k ops | folding the log is O(log), so per-op would be O(ops²) |
| million-op gate | once, against the final book | full scale |

Blueprint §9.2 asks for it after every operation at gate scale. That is O(ops²) and would take
the gate from four minutes to hours. The deviation is real, so it is written down here and in the
test's own comment rather than left for a reader to discover from the loop bounds. **A silently
sampled check reads as a total one.**

#### And it is tested against planted violations

Four plants, one per check: a fill the book performed but the log omits; an order accepted for
more than it really was; a cancel the book performed but the log omits; an order the log accepts
and the book never held. One of them originally sat behind `if (cxl < clean.size())`, which would
have passed silently on a stream with no cancels - now a `REQUIRE`. That is the same defect class
as D13 sizing the fuzz so exhaustion never fires.

Plus `conservation_survives_pool_exhaustion`, which runs a 16-slot pool specifically to force
rejections, and REQUIREs that it saw one before checking anything. That is **the path F4 was
hiding on**: an `OrderRejected` moves no quantity, so a half-accepted order breaks the balance.

### D23 - The two allocation sites that are not in Engine, and are still reachable from it (Phase 10b)

The README claimed *"two dynamic allocations exist in the whole engine, both at construction"*.
Wrong on both halves: there are four at construction, and two allocation sites remain reachable
from `apply`. Neither is *in* `Engine`, which is exactly why both were missed - "outside the class"
is not the same as "not on the hot path".

#### F9 - the caller's trade vector

`apply(const NewOrder&, std::vector<Trade>& out)` appends to a vector the CALLER owns. A sweep
deeper than its capacity reallocates: an unbounded allocation on the hot path, **the precise
defect D18 was rejected for**, sitting one indirection away and unnoticed while D18 was being
argued about.

The bound is the pool's, by the same argument that sized `IdIndex`: every trade but the last fully
consumes a resting maker, and at most `capacity` orders can rest. So `max_trades_per_apply()`
returns `pool.capacity() + 1`, and a caller that reserves it cannot make `apply` allocate.

Both rigs reserved **64** - a guess, and a guess that happens to hold is still a guess. They now
reserve generously and **verify the capacity did not change**, exiting with a loud INVALID rather
than reporting. That is the same rule that makes them refuse to run under a sanitizer, applied to
a defect they could previously have hidden. Verified: 0.00 new / 0.00 delete per operation across
all four profile modes with the check armed, and `bench_latency` completes without tripping it.

#### F10 - VectorSink

`push_back` per event. The important part is *why the obvious measurement would have missed it*:
`push_back` is amortised O(1), so allocations-per-event rounds to **0.00**, while the growth
reallocation is O(events so far) and events never stop. **Counting allocations per operation would
hide this exactly as it hid D18.** The metric that catches it is the max single operation, not the
mean - the same lesson as D21, in a different place.

It is a TEST sink, attached in no benchmark. It now has `reserve()`, and its header says plainly
that a caller attaching one in anger must either bound it up front or write a sink that does not
grow. The engine cannot bound this on their behalf, and pretending otherwise would be the third
version of the same mistake.

#### `Engine::book()` is const-only now

The non-const overload let any caller add to or remove from the book behind `retire()`'s back -
the one path that keeps book, index and pool in step (D14). Nothing outside `Engine` used it.
An accessor that hands out mutation rights nobody wants is a latent invariant break.

#### -Wreorder, and what it says about the audit

Turning `-Wall -Wextra` on over a clean build produced exactly one warning in the entire codebase,
and it was in code written the same day: `by_id_` declared before `levels_` but initialised after
it. C++ initialises in **declaration** order and ignores the member-init list's order, so the list
was misleading. Harmless as written - but it inverted an order that does matter, since
`checked_span()` validates the price window and should run before the index allocates.

The uncomfortable part is that a compiler flag found in one second what a careful audit did not.
`-Wall -Wextra` is not in the default build. It should be.

### D24 - The oracle as a performance baseline, which D6 promised and did not build (Phase 10b)
> **NUMBERS SUPERSEDED (D26), AND THE HEADLINE WAS WRONG.** The bolded **92x** below was the median
> of three hand-run invocations that were 54.8 / 92.9 / 95.6. The median is arithmetically correct;
> quoting a point estimate from a sample with that spread was not. An independent re-run measured
> 74-87x, and the rig now performs its own 5 runs and prints the min-max range, which shows depth
> 4,000 spanning 8-39x - not a quotable figure at any centre. Current table in `README.md`.
> The reasoning below - that the ratio's GROWTH is the claim, not the ratio - is unaffected.

D6 folded in a "cheap win": point the Phase 10 rig at `NaiveBook` so the tick-array and pool
choices go **from asserted to measured**. It was never built. Meanwhile the README kept asserting
exactly the two claims it was meant to settle:

> *"a tree is O(log n) pointer chasing over scattered heap nodes; an array index is arithmetic"*
> *"the allocator's worst case is unbounded, and unbounded is what disqualifies it, not slow"*

`NaiveBook` is the right strawman precisely because it was built for a different job. It is a
`std::map` per side with a vector per level and an O(n) scan to cancel, it shares no code with the
engine, and it is already differential-tested against it - so it is known to be *correct*, which
is what makes a speed comparison mean anything.

#### What is actually being claimed

**Not** "we are 90x faster". That would be a claim about `std::map`, and a dishonest one, since
`NaiveBook` is deliberately dumb. The claim is about **how the gap moves with depth**:

| depth | add ns/op (eng / naive) | cancel ns/op (eng / naive) | cancel ratio |
|---|---|---|---|
| 1,000 | 18.7 / 74.2 | 30.0 / 838 | **28x** |
| 2,000 | 17.5 / 64.1 | 34.1 / 989 | **29x** |
| 4,000 | 14.5 / 56.9 | 39.6 / 1,707 | **43x** |
| 8,000 | 18.2 / 57.3 | 44.8 / 3,380 | **75x** |
| 16,000 | 15.9 / 46.5 | 56.1 / 5,151 | **92x** |

Medians of 3 runs, g++ 11.4 `-O2 -DNDEBUG`, WSL2, same `rdtscp` timing as `bench_latency`.

- **Cancel is the complexity result.** Naive cancel roughly DOUBLES for each doubling of depth -
  800, 989, 1707, 3380, 5151 ns - which is the O(n) scan showing up exactly as it should. The
  engine's stays roughly flat, which is the id index being O(1). The *ratio* growing from 28x to
  92x over a 16x depth increase is the measurement; either single number is not.
- **Add is a constant factor, and honestly so.** ~3-4x throughout, not growing. A tree lookup over
  ~200 price levels is only ~8 comparisons, so O(log n) has barely started to hurt at this depth.
  Claiming the array indexing as a *complexity* win here would be overreach. It is a locality and
  branch-prediction win, and it is worth about 3x.
- **Naive add gets FASTER with depth** (83 down to 47 ns). Not an anomaly: at low depth most adds
  create new `std::map` nodes, and at high depth the levels already exist so it is a `push_back`
  into an existing vector. Stated because an unexplained downward trend in a benchmark is normally
  a bug, and here it is not.

#### The 4,159x number that was almost published

The first invocation reported naive add at **71,652 ns/op** at depth 1,000 - a 4,159x "win" - and
it did not reproduce on any later run. First-touch page faults on the first `NaiveBook`
constructed. `bench_latency` already warms up for this reason (D17) and this rig did not, because
it was written fresh rather than by copying the discipline from the rig next to it.

Worth recording because of how close it came to being a headline. It was spectacular, it was in
the right direction, and it would have been quoted. **A number that flatters the thing you built
deserves more scepticism than one that does not**, and the only reason this one was caught was
running the benchmark a second time before writing it down.

### D25 - Pre-ship adversarial audit: nine reachable defects, four of them same-day regressions
Three independent agents audited the code,
the tests and the documentation before the v0.1 tag. Every finding below was **demonstrated** by
the auditor with a running probe under ASan/UBSan, and then **re-verified against the source** here
before being accepted. The ship was blocked on them.

The uncomfortable headline: **the previous audit, one day earlier, missed all nine.** It looked for
contradictions between the code and the design log. It did not ask "what happens if a caller does
something the engine never does", and it did not consider exceptions at all.

#### D25.1 - `OrderType` outside {Limit, Market} is a null-pointer WRITE

The same field is classified five different ways, and they do not agree:

| decision | predicate |
|---|---|
| reserve a pool slot | `type == Limit` |
| validate price against the window | `type == Limit` |
| normalise the logged price | `type == Market` |
| never rest | `type == Market` |
| **rest it** | **`!(type == Market)`** |

`OrderType` is `enum class : uint8_t`, so `static_cast<OrderType>(2)` is a well-defined value. It
reserves no slot, skips price validation, is not Market so misses the early return, and reaches the
resting path with `slot == nullptr`. Under `-DNDEBUG`: `SEGV ... caused by a WRITE memory access`.

**Reachable from this project's own parser** - `tests/scenario.hpp` does `static_cast<OrderType>()`
on an integer parsed from text, so a malformed replay log crashes the engine. The scenario format
is the thing designed to ingest external data (LOBSTER), which makes this the input path most
likely to see hostile bytes.

The lesson is the table above: **five predicates over one field, written across four phases, and no
two phases checked each other.** Exactly D19's failure mode, in a form the D19 audit did not look
for.

#### D25.2 - The F4 fix caused a behaviour regression, and D19 costed it wrong

`apply()` acquires a pool slot for every limit order **before** `fill()` runs. So at pool
exhaustion, a marketable limit that would fully consume resting liquidity - and therefore *free*
slots - is rejected. That is precisely the order a venue most wants at capacity. Demonstrated: the
identical size sent as a Market order trades, while the limit is refused.

D19 states the cost of reserve-first as *"one acquire/release pair on the fully-filled path, and
both are pointer bumps"*. That sentence is true and it is not the whole cost. **It described the
cost in the common case and never asked what the change does at the boundary the change was about.**

`conservation_survives_pool_exhaustion` was written specifically to exercise exhaustion and did not
catch it, because it checks conservation - which holds fine - and not whether the rejection was
*correct*.

#### D25.3 - A throwing `EventSink` calls `std::terminate`

`emit()` is `noexcept`; `EventSink::publish()` is not. Any exception crossing that boundary
terminates the process. Not exotic: the shipped `VectorSink::publish` does `push_back`, so a
`bad_alloc` from the *test* sink kills the process. D23 documents that `VectorSink` reallocates
unboundedly and analysed it as a **latency** problem, never noticing the failure mode is
termination.

#### D25.4 - A throwing caller vector permanently leaks a pool slot

`slot` is a raw local. `fill()` calls `out.push_back()` on the **caller's** vector, which can throw.
The exception propagates out of `apply()` and the slot is never returned: `check_invariants()` goes
false and stays false, and repeating it drains the pool. `Engine::apply` had no exception safety
story at all, and nobody had asked for one.

#### D25.5 - `retire()` releases the slot even when the book refused it

D19/F6 recorded this as *"Now asserted"*. **An assert is not a fix under `NDEBUG`**, which is the
build that ships and the build that is measured. If `book_.remove()` returns false the release
happens anyway, freeing an order still linked into a level and still in the id index. Demonstrated:
`apply(Cancel)` returns **true** while leaving a dangling pointer in both, then ASan reports
use-after-poison through the index.

This is D8's own rule - *unconditional for memory corruption, assert for logic errors* - broken at
the call site that D19 wrote specifically to honour it.

#### D25.6 - `const OrderBook&` hands out a mutable `Order*`

`find()` is a const method returning a non-const pointer. D23 made `Engine::book()` const-only with
the comment *"a mutable handle lets a caller add or remove behind retire()'s back"*. `find()`
reopens exactly that door, one call later. Demonstrated: mutate `o->price` through the const
handle, then cancel - the order is unlinked from a level that does not contain it, corrupting that
level's links while remaining linked in its real one.

**Making the accessor const did not make the object const**, and the entry claiming otherwise was
written the same day.

#### D25.7 - `Quantity` is unbounded, and the invariant checker is structurally blind to it

`validate()` rejects only `quantity == 0`. Two orders of `2^63` at one price wrap
`total_quantity_`, and `depth_at()` then reports **0** while `2^64` rests - corrupt L2 market data
from two well-formed orders.

Worse: `check_invariants()` returns **true** throughout, because `PriceLevel::is_consistent()`
recomputes the sum with the *same* wrapping `uint64` arithmetic and therefore agrees with the
corrupted cache by construction. **A checker that recomputes a value the same way it was computed
cannot detect an arithmetic fault in that computation** - the same independence argument D22 made
for conservation, unapplied here.

#### D25.8 - `add()` accepts `id == 0` and permanently corrupts the index

`IdIndex` uses `id == 0` as its EMPTY marker; `Engine::kRejected` is also 0. `OrderBook::add()`
validates the price unconditionally - with a comment about D8's rule being *"applied in ObjectPool
and nowhere else"* - and then does not apply that rule to the id one line later. Under `-DNDEBUG`
the slot is written but still reads as empty, so it is never consumed while `count_` increments:
the order becomes unremovable, the pool slot leaks, and `count_` drifts past the load-factor bound,
which is the precondition for D25.9.

#### D25.9 - `IdIndex`'s probe loops are unbounded and HANG when the table fills

All three of `find`/`insert`/`erase` are `for(;;)` linear probes relying on an always-present empty
slot. The load-factor guarantee is an **`assert`**, deleted under `NDEBUG`, and
`OrderBook`'s `max_resting` **defaults to `1 << 16`** with no relation to any pool. Demonstrated
with `max_resting = 2`: a lookup of a missing id spins forever.

A hang is the worst failure mode a trading process has - no crash, no log, no core, and the
watchdog is the only thing that notices. D19 argued the sizing is safe because *"the pool says so"*,
which is true of `Engine` and is **a comment, not a mechanism**: `OrderBook` and `IdIndex` are
public types with a default that silently disagrees.

#### What the nine have in common

Two things, and neither is "a bug was written":

1. **Every guard that mattered was an `assert`.** D8 established the rule - unconditional for
   corruption, assert for logic errors - and D19 was written to apply it. Six of the nine are that
   rule not applied: the load factor, the id sentinel, `retire()`'s report, `reduce_front`,
   `push_back`'s precondition, the `resting != nullptr` claim. The rule was known, logged, cited,
   and unevenly applied.
2. **Nobody had considered exceptions.** `Engine::apply` can throw from three places and has no
   cleanup on any path. This was not an oversight in one function; there was no exception-safety
   position anywhere in the design log to be inconsistent with.

### D26 - The claims audit: what the documentation said that the code did not

A separate agent extracted every checkable factual claim from `README.md`, `SYSTEM-DESIGN.md`,
`WORKING-RULES.md` and the vault status note, and verified each against the code and a re-run of every
benchmark on the same machine. This matters more than usual because those documents are the source
for CV and interview copy.

#### The measurements did not reproduce

| claim | documented | re-measured |
|---|---|---|
| `cancel, hit` p50 | 87 ns | 176, 128 ns |
| `cancel, unknown` p50 | 35 ns | 83, 74 ns |
| throughput | 12.9 M ops/sec | 11.3, 12.2 M |
| baseline cancel ratio @16k | **92x** | 86.8, 85.2, 74.1, 76.2 |
| default suite runtime | 1.3 s | 2.8-3.1 s |
| run-to-run p99.9 spread | "roughly 5%" | 13-42% |

The `add` rows reproduce well; the `cancel` rows do not, which is the worst possible split given
that the entire positioning argument is *cancels are 90%+ of message traffic*.

**The 92x was the median of three runs that were 54.8, 92.9 and 95.6.** The median was computed
correctly. The error was quoting a point estimate at all from a sample with that spread, and doing
it in bold. This is the third instance of the same bias in one day, after D18's p50 headline and
D24's near-miss 4,159x - and it happened *after* the rule was written down.

**Root cause, and the actual fix:** `bench_baseline` runs each depth **once** per invocation, so
"medians of 3 runs" was something done by hand and recorded nowhere. `bench_latency` does its 5-run
median **inside the binary**, which is why its numbers held up better. Methodology that lives in
the operator's head is not methodology. The rig now does its own repetition.

#### The documentation was wrong about the code

- *"**Four** allocations at construction"* - it is **five** (`levels_`, `occupied_`, `IdIndex::table_`,
  `slab_`, `next_free_`). The sentence lists four items and then adds a fifth in the same breath.
  D23 exists to correct an earlier claim of "two". **The correction was also wrong.**
- *"depth at every price compared after each operation"* - the depth sweep runs **once, after** the
  100k loop. Per-operation depth comparison happens only in the two small non-gate cases.
- *"every trade field"* - `seq` is deliberately excluded. D13 documents the exclusion; the summaries
  dropped the qualifier.
- *"abort rather than report"* - `bench_profile` prints its results line and *then* checks. It
  reports first and flags second.
- *"shares no code with the engine, and therefore cannot share a bug"* - `naive_book.hpp` includes
  `me/engine.hpp` and uses its value types. It shares no *matching, storage or indexing* logic,
  which is the load-bearing claim; "shares no code" is literally false and is an easy gotcha.
- *"the engine's stays flat"* - contradicted by the table printed directly above it: 30.0 → 39.6 →
  56.1 ns, an 87% rise.
- *"seven invariants across ~1.1M operations"* - the 1M-op case checks **log properties only**. The
  invariants and the differential run over the 100k case. D16 states the split correctly; all three
  summaries compressed it.
- *"O(1) cancel"* - true for the lookup and unlink, but a cancel that empties the best level scans
  the occupancy bitmap at O(range/64). D20 says this honestly; the summaries dropped it.

### D27 - The checkers were trusted, not tested. Mutation testing found it; nothing else did.
Third of three pre-ship audits, and the only one that mutated the engine and re-ran the suite. It landed **after** the v0.1 tag was pushed,
because the tag went out when two of three had reported. That was a mistake and it is recorded
here rather than tidied away: the two audits that read the code found nothing in this area, and
the one that broke the code on purpose found all of it.

#### The finding, in one sentence

**Deleting any single clause of any invariant checker changes no test result.**

| checker | clauses neutered one at a time | detected by the suite or the gate |
|---|---|---|
| `OrderBook::is_consistent` | 9 | **0** |
| `PriceLevel::is_consistent` | 4 | **0** |
| `ObjectPool::free_list_is_consistent` | 2 | **0** |

Including invariant 1 (uncrossed book), invariant 2 (index points at this order), invariant 6
(nothing rests at zero), the free-list cycle detector, and the D20 bitmap check **whose own comment
says "neither is caught anywhere else"**.

`check_invariants()` is called tens of thousands of times per run and accounts for a large share of
the 763,621 gate assertions. **No test has ever planted a violation of any of the seven.** The file
applies exactly this discipline to `props::check` - *"A checker that never fails proves nothing"* -
and never applied it to the invariant checkers.

#### The gate passes an engine with time priority destroyed

`properties_hold_over_a_million_operations` calls `props::check` and `check_conservation` and
**never calls `check_invariants()`**. With `PriceLevel::push_back` pushing to the FRONT - LIFO, the
central guarantee of a price-TIME-priority book gone - the million-operation gate runs clean over
1,676,622 events. One added call would have caught it.

It also passes an engine whose **market takers skip the best level**, because `props::check`
deliberately applies no price constraint to a market taker. Only the 100k differential catches
that, at a tenth of the operations.

The Blueprint's stated accept criterion for Phase 7 is that million-operation run. It does not
constrain the two properties the whole design exists to provide.

#### 19 of 24 violation branches are undemonstrated

Neutering each `return fail(...)` in `properties.hpp` one at a time, only **5** are killed by any
planted violation. Undemonstrated branches include `properties.hpp:232` - which **is** Blueprint
§4.5, `accepted != filled + withdrawn + resting`.

Worse, the conservation plants are not what their comment claims. `the_conservation_checker_catches_
a_planted_violation` says *"One plant per check it makes."* All four plants produce the **same**
message from the **same** branch (the count comparison). The three checks carrying the actual
conservation content - per-order remaining, the indexed-sum cross-check, and §4.5's equation - have
no plant at all, and all three can be replaced with `if (false)` without changing the assertion
count by one.

That comment was written the same day, in the commit that added conservation. **Believing a test
does what its comment says is how all of this happened.**

#### Tests that pass with zero assertions

Two cases assert entirely inside `if (book.best_bid())` with no preceding `has_value()`. Break the
cursor - the exact defect they exist to catch - and they report `assertions: - none -` and pass. A
third runs 1 of its 3. Four more dereference `std::optional` unguarded, which is UB that neither
ASan nor UBSan diagnoses.

#### And the gate does not run under the documented command

`ctest --test-dir build` is what `CMakeLists.txt` and the test file both document.
`catch_discover_tests` does not enumerate hidden `[.gate]` tests, so the three most expensive cases
in the project are invisible to it. Following the README gives a green run with the entire Phase 7
gate omitted.

#### Two measurement findings

- **The throughput figure is about half instrumentation.** Same workload with and without the
  per-operation `rdtscp`+`lfence` pair: 12.99 M vs 28.01 M ops/sec. `lfence` is a serialising
  instruction. The latency block carries three caveats; the throughput number carried none.
- **`ctest --test-dir build-bench` runs the whole suite green** with `-O2 -DNDEBUG`, no sanitizers,
  and every `assert` compiled out. CMake warns loudly about MinGW Debug and says nothing about this.

#### What this changes about the project's claims

Not the architecture, and not the differential oracle, which is genuinely independent and caught
every mutant thrown at it. What it changes is the *strength* claimed for the assertion counts. The
honest position: **152,811 and 763,621 are counts of checks executed, not of behaviours
constrained.** A large fraction were checks that could not fail.

The repair is the one the project already knew about and applied in one place: plant a violation
for every branch of every checker, and make the headline gate call the checker that constrains time
priority.

### D28 - The perf pass, and the unbounded lookup it found in the structure built to remove one
2026-09-02. D6 defined v0.1 as including *"one honest `perf` pass"* and no counter output existed
anywhere in the repo: the Phase 10b evidence was allocation counts and latency percentiles.
`tools/perf-pass.sh` is that pass, written down so the numbers can be regenerated instead of
quoted from a terminal nobody still has open.

The pass found a defect within its first run, which is the best argument for having done it.

#### The finding: identity hashing made a miss O(resting orders)

The first `perf stat` showed `trade` mode at **76,000 instructions per operation** against ~250 for
`rest`, and `cancel_miss` so slow the run had to be killed. That is not a tuning observation, it is
a broken cost curve.

D19 chose an identity hash for `IdIndex`, reasoning that ids are strictly increasing so masking
*"distributes them perfectly AND keeps recently-issued ids adjacent"*. Both halves are true, and
the conclusion is backwards: **perfect adjacency is maximal clustering.** Every live entry occupied
one contiguous run of slots, and linear probing terminates at the first EMPTY slot, so any lookup
whose home landed inside that run had to walk to the end of it.

Measured against `IdIndex` directly, miss lookups:

| live entries | miss landing outside the run | miss landing inside it |
|---|---|---|
| 10,000 | 2.0 ns | **6,172 ns** |
| 40,000 | 1.1 ns | **27,216 ns** |
| 320,000 | 1.2 ns | **241,035 ns** |

Linear in the number of resting orders. **241 microseconds for a single cancel lookup.**

And reachable through the public API with nothing malformed - 3,000 resting orders, one large id,
**60 ns to 1,793 ns**. D19 said it was *"safe because ids are never client-supplied"*; an id does
not need to be chosen, it only needs to ALIAS into the block, and **the engine issues such ids
itself as soon as `next_id_` passes the table size**, which any real session does in minutes.

> [!warning] This is D18's rule, inside the structure D19 built to satisfy it
> *"The allocator's worst case is unbounded, and unbounded is what disqualifies it, not slow."*
> `IdIndex` replaced an unbounded reallocation with an unbounded probe, and the entry recording
> that work argued the hash was safe on grounds that were never tested. Three audits read that
> paragraph and none of them checked it. Running the code did.

#### The fix, and why the obvious one was wrong

**Plain Fibonacci hashing removes the tail and costs 2-3x on the common path.** Measured: `add,
rested` p50 39 -> 117 ns, throughput 24-30 M -> 14 M ops/sec. The cause is that the table is sized
from POOL CAPACITY while a book usually holds far fewer orders, so scattering a few thousand live
entries across a 2M-slot (33 MB) table makes every lookup a cache miss. **The identity hash was
dodging that by accident** - packing everything into one small hot region is exactly the clustering
that created the tail.

So: **scatter the block, keep ids adjacent within it.** Consecutive ids share a 64-slot block;
successive blocks land far apart. Locality is bounded below by the block, probe length bounded
above by it.

| | add ns/op @16k | cancel ns/op @16k | worst miss probe | tail |
|---|---|---|---|---|
| identity | 19.6 | 98.6 | **~live (64,000)** | **unbounded** |
| Fibonacci | 56.3 | 43.9 | 1 | bounded |
| **block-scatter** | **18.7** | **55.6** | **64** | **bounded** |

The regression test counts PROBES rather than timing anything, because the property is structural
and a stopwatch in a test suite is flaky. It asserts the worst probe stays under 128 - one block
plus spill - at 1,000 through 64,000 live entries. Under the old hash the last of those was 64,000.

Cost accepted: cancel is slower than identity's best case at shallow depth (177 ns vs 120 p50),
because a scattered probe is a cache miss where a clustered one was not. That is the price of a
bounded tail and it is the trade this project's own rule mandates.

### D29 - What the counters actually say
2026-09-02. g++ 11.4 `-O2 -DNDEBUG`, WSL2, 200,000 resting orders, 200,000 measured operations,
median of 3. Counters are per operation, with the fill-only `none` mode SUBTRACTED - aggregate
subtraction, not per-region counting, so a few percent between modes means nothing.

| mode | cycles | instructions | IPC | cache misses | branch misses |
|---|---|---|---|---|---|
| rest | 117 | 249 | 2.13 | 3.0 | 0.005 |
| trade | 105 | 239 | 2.28 | 0.5 | 0.009 |
| **cancel** | **722** | **363** | **0.50** | **13.2** | **1.24** |
| cancel, miss | 21 | 60 | 2.82 | 1.2 | ~0 |

Three things worth saying, and only one of them is a number.

**Cancel is memory-bound and nothing else is.** IPC 0.50 against 2.1-2.8 everywhere else, on
*fewer* than 400 instructions. It is not doing more work, it is waiting: 13 cache misses per
operation against 0.5-3. The pointer chase is inherent to the design - find the node through the
index, then touch its predecessor and successor to unlink it, then the pool's free list, then
possibly the occupancy bitmap - and each of those is a separate cache line that nothing warmed.
This is the one place where a v1.5 optimisation has a measured case behind it rather than an
argument. It is NOT being done now: D6's gate is one honest pass, and inventing work off the back
of it is how a measurement pass turns into a rewrite.

**Branch prediction is a non-issue.** 0.005 to 0.009 misses per operation on the add and trade
paths. The matching loop's branches are almost perfectly predicted, so the `<=` versus `<` crossing
logic and the side dispatch cost essentially nothing. Any proposal to make this code branchless
should be refused unless it comes with a counter that contradicts this.

**The cache-miss counts vindicate D7 rather than the tick array.** `rest` touches 3 lines per
operation, which is about what a one-cache-line `Order` plus its level header and index slot should
cost. `alignas(64)` was justified in D7 by a straddle count and never by a miss count; this is that
number.

Limits of the environment, stated because they bound what can be concluded: `L1-dcache-*` and
`LLC-*` are **not exposed under WSL2** (`<not counted>` / `<not supported>`), so `cache-references`
and `cache-misses` are the whole cache picture and the level at which they miss is unknown.
`/usr/bin/perf` refuses on this kernel and the versioned binary underneath is what works;
`perf_event_paranoid` is 2 so every event is userspace-only, which is what is wanted anyway.


### D28 - The reject path returns the reason, not a sentinel (v1.5)

`Engine::apply(NewOrder)` returned `OrderId`, with `kRejected = 0` standing in for "refused".
The caller learned THAT an order failed and never WHY - even though `validate()` had already
computed a typed `RejectReason` one line earlier and thrown it away at the boundary. The
information existed; the signature had nowhere to put it.

It now returns `std::expected<OrderId, RejectReason>`, marked `[[nodiscard]]`.

#### Alternatives considered

| Option | Why not |
|---|---|
| Keep the sentinel, add a `RejectReason*` out-parameter | Two return channels for one outcome, and nothing forces a caller to pass it. The information stays optional, which is how it got lost the first time. |
| Throw on reject | A reject is **routine**, not exceptional - a malformed or out-of-band order is ordinary traffic at a venue. Putting a throw on a normal branch of the hot path trades a return value for stack unwinding. |
| `std::optional<OrderId>` | Says "no id" without saying why. That is the sentinel with better manners. |
| Convert `apply(Cancel)` to `expected<void, RejectReason>` too | **Deliberately not done.** It has exactly one failure mode, `UnknownOrder`, already named in the event it emits, so the expected could hold precisely one error value and would carry nothing a `bool` does not. It also stays `noexcept`. Symmetry is not a reason. |

#### What `[[nodiscard]]` bought, concretely

The attribute is the half that pays. Without it a caller can drop the reason exactly as before, and
the change is decorative. With it the compiler enumerated every site that had been discarding the
outcome: **46 in the test suite, 4 in `bench/latency.cpp`, 1 in `tests/scenario.hpp`, 3 in the
profile and baseline shims.**

Two of those were places where the sentinel had been quietly load-bearing:

- `bench/profile.cpp` counted rejects with `id == kRejected`. It now asks `!id.has_value()`, which
  is the same test written in a form that cannot be confused with a valid id.
- `bench/baseline.cpp`'s `EngineAdapter` and the differential harness both compare against
  `NaiveBook`, which still speaks the 0-means-rejected dialect. They fold with `value_or(0)`,
  named at each site. The oracle was left alone on purpose: it earns its keep by being the dumb
  implementation you can check by eye, and mirroring the engine's return type would make it
  marginally less independent.

Three reject tests now assert **which** reason fired (`MalformedOrder` for a bad `OrderType`, for a
bad `Side`, and `InvalidQuantity` for the over-cap quantity) rather than "not accepted". That is a
strictly stronger assertion: the old form passed if the order was refused for any reason at all,
including the wrong one.

#### What it cost

**The compiler floor moved to g++ 12.** `std::expected` is libstdc++ 12; Ubuntu 22.04 defaults to
11.4. `CMakeLists.txt` now refuses at configure time with the fix in the message, rather than
letting it fail as several screens of template errors that never name the cause.

The C++23 position changes with it. `std::to_underlying` in the log serialiser was the only
C++23 library feature in the codebase; there are now two, and the second is the one Blueprint §7
called the flagship.

#### The sentinel's other half survives, and is now in the right place

`0` is still never issued as an order id, because `IdIndex` uses it as its EMPTY marker. That
constraint had been conflated with "rejected" - one magic value carrying two unrelated meanings in
three files. `kRejected` is gone; `next_id_` still starts at 1, and the comments in `id_index.hpp`
and `order_book.hpp` now give the actual reason.

**Revisit trigger:** if a rejection ever needs to carry more than one datum - which field, what
bound - `RejectReason` becomes a struct and `expected` already has somewhere to put it. That was
not true of the sentinel.

#### F26 - FIXED 2026-09-02: `IdIndex::home()` shifted by more than 63 for small tables

Found by UBSan while running the suite for this change, in code this change does not touch.

```
id_index.hpp:167: runtime error: shift exponent 65 is too large for 64-bit type
```

`shift_` is `64 - log2(table_size)` and `home()` shifts by `shift_ + 6`. The shift is only
defined while `shift_ + 6 <= 63`, i.e. `table_size >= 128`, i.e. **pool capacity >= 64**. Several
tests construct `Engine(kMin, kMax, 1)` and `(…, 64)`; capacity 1 gives a 2-slot table, `shift_`
63, and a shift of 69.

On x86 the hardware masks the shift count to 6 bits, so `>> 69` executes as `>> 5`: a
wrong-but-deterministic block index. That is why every test still passes and why the differential
never caught it - correctness does not depend on hash quality, only on the hash being a function.
What it does undermine is the **bounded-probe argument of D19**, which assumes the intended
scatter, and it is undefined behaviour that a different optimiser is free to treat differently.

The trigger predates this change: the code and the capacity-1 tests were both already there.

#### The fix, and why the threshold is 58 rather than a patched-in number

When the table holds at most one 64-slot block there is no block to scatter, so the block index
should be zero and the intra-block term does all the work - which degenerates to exactly the
neighbours-stay-together scheme that is correct at that size, since a table of 64 slots or fewer
has nowhere for a run to pile up.

```cpp
const std::uint64_t blk = (shift_ >= 58)
                        ? 0
                        : ((v / kBlock) * kPhi) >> (shift_ + 6);
```

`shift_ >= 58` says "the table is 64 slots or fewer". The two conditions that matter turn out to
be the same condition, which is the sign this is the real boundary and not a clamp:

| capacity | table | `shift_` | shift amount | |
|---|---|---|---|---|
| 1 | 2 | 63 | 69 | undefined → guard, `blk = 0` |
| 32 | 64 | 58 | 64 | undefined → guard, `blk = 0` |
| 64 | 128 | 57 | 63 | legal, and 63 is the largest legal shift |

Capacity 64 is both the first size at which a second block exists and the first size at which the
shift is defined. One comparison covers both because they are the same fact.

**A branch was added to `home()`, which is on the lookup path.** `shift_` is fixed for the life of
the index, so the branch should predict perfectly after the first call - but *should* is an
argument, not a measurement, and this log has been wrong that way before (D21). So it was measured:
both binaries kept, interleaved in one session, five runs each.

| | p99.9 median | p50 median | throughput median |
|---|---|---|---|
| before the guard | 481 ns | 49 ns | 21.5 M ops/sec |
| after the guard | 465 ns | 49 ns | 21.2 M ops/sec |

**No measurable cost.** p50 is identical, and the two remaining differences point in opposite
directions - the tail median came out 16 ns *faster* with the extra branch, which a branch cannot
do. That contradiction is the useful part of the result: it says the spread is dominating, not the
change. Ranges overlap almost entirely (478-525 against 438-503; 20.6-25.1 M against 21.0-24.7 M).

The alternative - folding the shift into a precomputed mask so no branch exists - was not pursued:
there is no measured cost to remove, and it would trade a line anyone can read for one they cannot.

#### Verification

- The UBSan diagnostic is **gone**: the identical command that printed
  `id_index.hpp:167: runtime error` now prints nothing.
- Suite still green: 88 cases, 152,953 assertions.
- Phase 7 gate re-run after the fix: **758,717 assertions, 3 test cases, exit 0**.


---

### D30 - Continuous integration: what it is for here, and what it is deliberately not for

> **Written BEFORE implementation (2026-09-03), on purpose.** Every prior entry in this file is a
> retrospective. This one is a commitment made in advance, so that when it is built the record can
> be checked against it rather than written to flatter it - the failure mode D21 and D24 are both
> about. If the workflow that ships differs from this, amend the entry and say why.

The repository has **no CI at all** - there is no `.github/` directory. Every claim it makes is
therefore true only of one machine: "ASan clean", "the suite is green", "it builds under g++ 12",
and the entire measurement story. `tools/perf-pass.sh` was written because numbers "quoted from a
terminal nobody still has open" are not evidence. That argument applies with equal force to the
build, and has not been applied to it.

**Decision: add GitHub Actions running the Debug (ASan + UBSan) build and `ctest` on a clean Ubuntu
runner, across a two-compiler matrix.**

#### What it is for, in order of actual value

1. **Enforcing the C++23 compiler floor.** This is the reason that justifies the rest. D28 moved
   the floor to g++ 12 for `std::expected`, and D20 established what "C++23" means here. Today that
   floor lives in a `CMakeLists.txt` configure check and in prose. A matrix building on **g++-12 and
   g++-13** makes it a property the machine enforces: add a feature needing g++ 14 and it goes red.
   This is the single claim in the project that has been re-litigated most, so it is the one worth
   mechanising.
2. **Portability of the correctness claim.** A fresh runner has no WSL2 quirks, no stale
   `build-bench11/`, no locally-installed toolchain. "Passes on a machine that is not mine" is a
   strictly stronger statement than the one currently on offer.
3. **Evidence.** The repo is public and linked from the CV. A green badge is the cheapest
   conversion of existing work into a visible signal available.

Point 3 is real and should be stated rather than dressed up as engineering. On a **solo repository
with no collaborators, CI catches almost no defects you would not catch anyway**, because you
already run the full gate locally before tagging. Its value here is evidential and reproducibility,
not defect detection. Claiming otherwise would be the same self-flattery D21 recorded.

#### The six sub-decisions

| # | Decision | Reasoning |
|---|---|---|
| a | **Debug build type only. No TSan.** | Debug is the type carrying ASan and UBSan per D4. TSan looks for data races; the engine is single-threaded by design per D6, so TSan can find nothing today. Running it would be coverage theatre. **Add it when the SPSC ring lands in v1.5** - it becomes load-bearing at exactly that moment and not before. |
| b | **Split fast and full: `ctest -E gate` on every push, full `ctest` on PR + nightly.** | The Phase 7 gate is **6m22s** under sanitizers (758,717 assertions). The README makes it non-skippable locally on purpose, and that stays. But six minutes is long enough that you stop watching, and an ignored signal is worse than none. The split buys a seconds-long loop without ever letting the gate go unrun. |
| c | **Matrix on g++-12 and g++-13; install both explicitly.** | See above. **Do not rely on the runner image's default gcc** - it changes without notice, which would silently move the floor being tested. Pinning is the entire point of the job. |
| d | **Build the bench targets; assert nothing about their numbers.** | Runners are shared, virtualised, unpinned and noisy. A p99.9 threshold would flap red on load unrelated to the code, and a flapping alert trains you to ignore it. Compilation is checked; measurement stays on a controlled machine and is quoted as a **band**, per D21 and D29. |
| e | **Cache the `_deps` directory, keyed on compiler + `hashFiles('CMakeLists.txt')`.** | Catch2 arrives via `FetchContent` and a cold runner rebuilds it every run. **The compiler must be in the cache key** - without it a stale entry can serve a Catch2 built by a different compiler and produce a green run that means nothing. That is the classic CI footgun and it is worth naming here rather than discovering it. |
| f | **`concurrency` group with `cancel-in-progress`.** | Three pushes in a row otherwise queue three full runs against code already superseded. |

#### Alternatives considered

| Option | Why not |
|---|---|
| **No CI - keep running the gate locally before tagging** | The honest baseline, and it is not obviously wrong: on a solo repo it catches nearly the same defects for zero maintenance. It is rejected only because it cannot make the compiler-floor claim mechanically, and cannot make any claim portable off one machine. If (a) and (2) above stop mattering, this option becomes correct again. |
| Run the full `ctest` including the gate on every push | Simplest possible config, and free on a public repo. Rejected on **feedback latency**, not cost: 6m22s per push is where people stop reading their own CI. |
| Single compiler (`g++-12` only) | Halves the runs, and proves the floor holds - but not that anything *above* it still works. The matrix is the cheap half of the value. |
| Gate on benchmark regression (fail if p99.9 exceeds a threshold) | Tempting, and wrong for this environment. See (d). Would produce exactly the flattering-or-flapping numbers D21 and D24 are about. |
| Self-hosted runner on a pinned machine, so benchmarks *could* be gated | The only way to make benchmark CI meaningful. Rejected as far beyond the value: it means maintaining a machine, and the measurement discipline already documented is sufficient. Revisit only if the engine acquires users who depend on its latency. |
| Add a deploy or release-artifact step | Nothing consumes this as a binary. Pure surface, no consumer. |

#### What it costs - the tradeoffs, stated plainly

1. **A new failure surface that is not the engine.** CI breaks for apt mirror flakiness, a runner
   image bumping its defaults, a deprecated action version. Each is a red X on a commit that is
   fine, and each costs time spent on the pipeline instead of on the code.
2. **Red-badge decay is the real risk, and it is worse than no CI.** A permanently-red badge on a
   public repo linked from a CV is a negative signal shown to exactly the audience the badge was
   added for. This is the failure mode to watch.
3. **It does not validate any performance claim, and cannot.** The numbers come from WSL2; a runner
   is a different virtualised environment. CI proves "builds and passes clean", which is worth
   having, and nothing at all about latency. Two environments now exist and neither is a pinned
   machine - a mild increase in "which environment was that?" ambiguity, which this project has
   already been burned by twice.
4. **Sanitizer wall-clock grows with the suite.** 6m22s today. The TSan build in v1.5 adds another.
   The split in (b) mitigates but adds config to maintain.
5. **The matrix multiplies flake surface.** Two compilers times two job types is four runs per push.
   Free, but four chances for an unrelated failure instead of one.
6. **Opportunity cost, and it is the one that actually matters.** An afternoon on CI is an afternoon
   not spent on interview preparation, and the internship research is explicit that the gate at
   Big Tech interns is the online assessment, not the CV. CI is worth doing because the engine wants
   it anyway. It is **not** worth doing as an application tactic, and must not delay a submission.

#### What would falsify this decision

- **If after one month the badge has been red more often than green for reasons unrelated to the
  engine**, the pipeline costs more than it returns. Delete it rather than nurse it. A deleted
  workflow is a clean state; a nursed one is a tax.
- **If the gate ever gets skipped, narrowed or `-E`'d out of the full job to make a run go green**,
  it has become theatre and the entry above is void. That is the same disease D27 found in the
  assertions: a check that cannot fail is not a check.
- If a second contributor ever appears, points 1-3 of "what it is for" reorder and defect detection
  becomes the primary value. Re-read this entry then; it was written for a solo repo.

**Revisit trigger:** v1.5, when the SPSC ring buffer lands. That is when TSan becomes real, when
concurrency bugs become possible, and when CI stops being mostly evidential and starts earning its
keep as a defect gate.

#### Amendment on implementation (2026-09-03, same day)

The entry above asked to be checked against what actually shipped. `.github/workflows/ci.yml`
diverges from it in three places, all recorded here rather than by quietly editing the decision.

**1. `ctest -LE gate`, not `-E gate`.** Sub-decision (b) named the wrong flag. `-E` is a
**name** regex; `-LE` is a **label** exclusion. The gate is registered with
`set_tests_properties(phase7_gate PROPERTIES LABELS "gate")`, and `-LE gate` is the opt-out
this file's own CMakeLists comment already documents. `-E gate` happens to work today only
because the test's *name* also contains the substring - so it would silently exclude any
future test named `..._gate_...` that was never meant to be skipped. The correct flag was
already in the repo; the decision entry simply misquoted it.

**2. The runner image is pinned to `ubuntu-24.04`, not `ubuntu-latest`.** Sub-decision (c)
pinned the compilers but left the image floating, which is the same mistake one level up:
`latest` is a moving alias, and an image bump could change which gcc versions apt can offer at
all. This stopped being hypothetical during implementation - **the development machine runs
Ubuntu 22.04, whose repositories carry g++-12 but have no g++-13 candidate.** So the matrix
this workflow exists to run cannot be reproduced locally, which is simultaneously the strongest
argument for it and the reason its environment must be pinned rather than inherited.

**3. `-Werror` was considered and rejected; D30 never raised it.** The build already sets
`-Wall -Wextra -Wconversion` (deliberately after `FetchContent`, so it warns on our code and not
Catch2's). Promoting those to errors is a change to the project's build policy, not a CI
decision, and making it silently inside a workflow file would be exactly the kind of drift this
log exists to prevent. Left as a warning. If it is ever wanted, it belongs in `CMakeLists.txt`
under its own entry.

#### What was verified before committing, and what was not

**Verified by execution:**
- The YAML parses, and defines the three jobs intended.
- **The label exclusion does what is claimed.** Against the real test list on this machine:
  `ctest --test-dir build -N` reports **90 tests** with `phase7_gate` as #90;
  `ctest --test-dir build -N -LE gate` reports **89**, and `phase7_gate` is the one dropped.
- Local toolchain inventory: Ubuntu 22.04.5, default `g++` is 11, `g++-12` installed (12.3.0),
  `g++-13` has no apt candidate, **`ninja` is not installed at all** - even though the
  CMakeLists header and README both document `-G Ninja`. The documented invocation therefore
  does not currently run on the machine it was written on. Not fixed here; noted.

**NOT verified - and this is the honest limit:** the workflow has never been executed. Nothing
local can run a GitHub runner. If the `ubuntu-24.04` label is stale, or either g++ package is
missing from that image, or a restored cache misbehaves, **the first run is where it surfaces**.
That first run is the verification step, not this entry.

#### Housekeeping flag, noticed while writing this

**There are two entries numbered D28** - "The perf pass, and the unbounded lookup it found in the
structure built to remove one" and "The reject path returns the reason, not a sentinel (v1.5)".
Not corrected here, because renumbering would break every reference to D28 elsewhere in the repo.
Recorded so the collision is known rather than discovered later. This entry is D30; there is no D31
pending.

---

### D31 - The compiler floor is selected by a preset, not by the build system or the machine

D28 moved the compiler floor to g++ 12 and added a configure-time guard that refuses anything
older with the fix in its message. That guard works - it fired on the development machine on
2026-09-03, where the system `c++` is 11.4 and `g++-12` is installed alongside it. But every
correct invocation of this project now has to carry `-DCMAKE_CXX_COMPILER=g++-12` by hand, and
a flag you have to remember is a flag you will eventually forget.

**Decision: add `CMakePresets.json` defining named configure, build and test presets that carry
the generator, the build type and the compiler.** `cmake --preset debug` replaces the full
incantation.

#### Alternatives considered

| Option | Why not |
|---|---|
| **Set `CMAKE_CXX_COMPILER` in `CMakeLists.txt`** before `project()` | The obvious move and the wrong one. It would hardcode one compiler at one path into the build system: the CI matrix could no longer vary the compiler, which is the entire reason D30 built it; anyone without g++-12 at `/usr/bin/g++-12` gets a hard failure instead of the guard's actionable message; and clang or a newer gcc becomes unusable without editing the file. **The build system should state a floor and refuse below it, which it already does. It should not choose.** |
| `update-alternatives` to make gcc-12 the system default | Fixes every project on the machine with no flags anywhere, and is reversible. Rejected because it is **invisible machine state**: a fresh clone on any other box hits the identical error, so the project is no more portable than before, and every unrelated project on the machine silently moves off gcc 11. It solves the symptom on one computer. |
| `export CXX=g++-12` in the shell profile | Same objection at a smaller blast radius, plus CMake caches the compiler per build directory on first configure, so the variable's effect is sticky and easy to misattribute later. |
| Leave it, and keep typing the flag | The status quo. It is not wrong - the guard makes the failure mode loud rather than silent. Rejected on ergonomics alone, which is a weaker reason than most entries in this file rest on, and worth admitting as such. |
| Add a `tsan` preset too | **Deliberately not done.** The TSan build type exists, but the engine is single-threaded by design (D6) so TSan can find nothing today - the same reasoning that kept it out of CI in D30. Add the preset in v1.5 alongside the SPSC ring, when it starts being able to fail. |

#### Shape

Configure presets are named for the build type they select and pin generator, type and compiler
together, because those three are what actually have to agree. `debug` and `bench` use the same
`build/` and `build-bench/` binary directories the README already documents, so nothing else in
the documentation goes stale. A second `debug-g++-13` mirrors the CI matrix so a CI-only failure
can be reproduced by name rather than reconstructed.

Test presets carry the split D30 made: `fast` excludes the `gate` label, `all` excludes nothing.
That puts the fast/full distinction in one committed place instead of in a flag that has already
been misquoted once in this log.

#### What it costs

1. **Ninja becomes a hard prerequisite**, where before it was only documented. `cmake --preset
   debug` fails outright without it. That is arguably a fix - the CMakeLists header and README
   have documented `-G Ninja` all along on a machine that never had it installed - but it is a
   new way for a first run to fail.
2. **Existing build directories were configured with Make and will reject a Ninja preset**, since
   CMake refuses to change generator in place. `build/` can be deleted freely; `build-bench*/`
   hold binaries behind published numbers and should be renamed rather than removed.
3. **A second place that names `g++-12`.** The floor is now asserted in `CMakeLists.txt` (as a
   refusal), in `.github/workflows/ci.yml` (as a matrix), and in `CMakePresets.json` (as a
   default). Three files must agree. They express different things - refuse, test, prefer - so
   this is not pure duplication, but it is a drift surface and is recorded as one.
4. **`debug-g++-13` cannot run on the development machine at all** (Ubuntu 22.04 has no g++-13
   candidate). It exists for CI parity and will fail locally with a compiler-not-found error,
   which is honest but will look like a broken preset to anyone who tries it cold.

#### What would falsify this

If the three files naming the floor ever disagree - the guard says one version, CI tests another,
the preset prefers a third - the duplication in cost (3) has stopped being expressive and become
a bug. At that point the floor wants a single source, most likely a variable in `CMakeLists.txt`
that CI and the presets both read.

**Revisit trigger:** v1.5. The `tsan` preset lands with the SPSC ring, and if a fourth file ends
up naming the compiler floor, consolidate instead of adding.

---

## Open questions (from the Blueprint's critique — decide as you reach them)
- Best-price cursor advance: linear scan vs occupancy-bitmap + `countr_zero`? (§3.2)
- Cancel/replace on amend: keep the old order id or mint a fresh one? (§5.5)
- Object-pool exhaustion policy: reject, or grow? (§10g)
- Client-supplied vs engine-assigned order ids? (§10g)
