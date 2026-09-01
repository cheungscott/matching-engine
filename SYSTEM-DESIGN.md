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

### D10 - OrderBook: one tick-indexed array, sentinel cursors (Phase 1)
**, 2026-09-01.**

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
**, 2026-09-01.**

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
**, 2026-09-01.**

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
**, 2026-09-01.**

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
**, 2026-09-01.**

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
**, 2026-09-01.**

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

### D16 - Property tests and the oracle fuzz, without RapidCheck (Phase 7)
**, 2026-09-01.**

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
**, 2026-09-01.**

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
**, 2026-09-01. Recorded because it was WRONG.**

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
**, 2026-09-01**, after an adversarial audit found 25 issues,
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
**, 2026-09-01.**

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
**, 2026-09-01.**

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

---

## Open questions (from the Blueprint's critique — decide as you reach them)
- Best-price cursor advance: linear scan vs occupancy-bitmap + `countr_zero`? (§3.2)
- Cancel/replace on amend: keep the old order id or mint a fresh one? (§5.5)
- Object-pool exhaustion policy: reject, or grow? (§10g)
- Client-supplied vs engine-assigned order ids? (§10g)
