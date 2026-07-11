# Matching Engine

A single-symbol, price-time-priority limit order book in **C++23**.

> **Status: design stage — Phase 0 scaffold only. Not built yet.** The matching
> logic is intentionally unwritten; this repo is the skeleton the real work
> hangs on.

## What this is

A matching engine demonstrating **latency discipline +
correctness under concurrency**:

- Order book = price-indexed array of levels, each an **intrusive doubly-linked list** (O(1) add / cancel, zero hot-path allocation)
- Ingress = **lock-free SPSC ring buffer** (LMAX / single-writer principle)
- **Single-writer** matching thread → determinism *and* data-race-freedom without locks
- Append-only **sequenced event log** → byte-identical replay
- RapidCheck property tests · HdrHistogram latency on a pinned isolated core · LOBSTER replay (stretch)

## Design docs (source of truth)

Architecture and the phased build plan live in separate design notes. `SYSTEM-DESIGN.md`
here is the running decision log.

## Build

**Right now (g++ only: MinGW g++ 15.2, no cmake yet):**

```sh
g++ -std=c++23 -Wall -Wextra -Wconversion -Iinclude tools/smoke.cpp -o smoke && ./smoke
```

**Full path (after `winget install Kitware.CMake`, plus a generator like Ninja):**

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Toolchain gotcha (found during Phase 0)

`std::print` / `<print>` does **not link** on this MinGW g++ 15.2 build
(undefined reference to `std::__open_terminal`). Use `std::format` + `std::cout`
or fmtlib instead. This is the "check compiler support" caveat from Blueprint §7,
confirmed on this toolchain.

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
single-threaded. See Blueprint §11.
