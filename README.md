# Matching Engine

A single-symbol, price-time-priority limit order book in **C++23**.

> **Status: design stage — Phase 0 scaffold only. Not built yet.** The matching
> logic is intentionally unwritten; this repo is the skeleton the real work
> hangs on. (Honesty rule: never imply it's finished until it is.)

## What this is

An interview-defensible quant-dev artifact demonstrating **latency discipline +
correctness under concurrency**:

- Order book = price-indexed array of levels, each an **intrusive doubly-linked list** (O(1) add / cancel, zero hot-path allocation)
- Ingress = **lock-free SPSC ring buffer** (LMAX / single-writer principle)
- **Single-writer** matching thread → determinism *and* data-race-freedom without locks
- Append-only **sequenced event log** → byte-identical replay
- RapidCheck property tests · HdrHistogram latency on a pinned isolated core · LOBSTER replay (stretch)

## Design docs (source of truth)

Architecture and the full phased build plan live in the Obsidian vault:

- `System/Internship/Matching-Engine-Design.md` — architecture (the decisions)
- `System/Internship/Matching-Engine-Blueprint.md` — build answer-key (phases, interfaces, invariants, gap-fixes)

See `SYSTEM-DESIGN.md` here for the running decision log.

## Build

**Right now (g++ only — you have MinGW g++ 15.2, no cmake yet):**

```sh
g++ -std=c++23 -Wall -Wextra -Wconversion -Iinclude tools/smoke.cpp -o smoke && ./smoke
```

**Full path (once you `winget install Kitware.CMake` + a generator like Ninja):**

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Toolchain gotcha (found during Phase 0)

`std::print` / `<print>` does **not link** on this MinGW g++ 15.2 build
(undefined reference to `std::__open_terminal`). Use `std::format` + `std::cout`
or fmtlib instead. This is the "check compiler support" caveat from Blueprint §7,
confirmed on your actual toolchain.

## Layout

```
include/me/types.hpp   value types (Order, Trade, Side, ...)   ← Phase 0
tools/smoke.cpp        framework-free smoke test (g++ now)      ← Phase 0
tests/test_types.cpp   Catch2 suite (grows from Phase 1)        ← Phase 0
src/                   engine implementation                    ← Phase 1+
CMakeLists.txt         build config (C++23, ASan/UBSan, Catch2)
```

## Next: Phase 1

Rest & match one order on the target structures (intrusive list + tick-array),
single-threaded. See Blueprint §11. **You write the matching logic** — the
blueprint is the answer-key to check against, not to transcribe.
