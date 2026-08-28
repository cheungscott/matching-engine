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

### D6 - v0.1 gate re-cut: correctness + measurement, concurrency deferred (2026-08-27)
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
- **Revisit trigger:** if Phases 1-7 are green before Sun 30 Aug, Phase 8 (ring in
  isolation, TSan) re-enters scope - it is self-contained and does not touch the
  book.

---

## Open questions (from the Blueprint's critique — decide as you reach them)
- Best-price cursor advance: linear scan vs occupancy-bitmap + `countr_zero`? (§3.2)
- Cancel/replace on amend: keep the old order id or mint a fresh one? (§5.5)
- Object-pool exhaustion policy: reject, or grow? (§10g)
- Client-supplied vs engine-assigned order ids? (§10g)
