#!/usr/bin/env bash
# tools/perf-pass.sh — the Phase 10b hardware-counter pass, reproducibly.
#
# D6 defined v0.1 as including "one honest perf pass" and no counter output existed
# anywhere in the repo: the Phase 10b evidence was allocation counts and latency
# percentiles. This script is that pass, written down so the numbers can be
# regenerated rather than quoted from a terminal nobody still has open.
#
# TWO THINGS TO KNOW BEFORE READING ANY NUMBER IT PRINTS
#
# 1. perf counts the WHOLE PROCESS. bench_profile builds a million-order book before
#    its measured loop, so a naive `perf stat ./bench_profile rest` is roughly half
#    setup. The `none` mode does the fill and skips the loop, and its counters are
#    SUBTRACTED. That is aggregate subtraction, not per-region counting: it is an
#    approximation, and a difference of a few percent between modes means nothing.
#
# 2. /usr/bin/perf refuses on WSL2 because it looks for a build matching the
#    Microsoft kernel version, which Ubuntu does not package. The versioned binary
#    underneath works and reads real counters. perf_event_paranoid is 2, so every
#    event comes back userspace-only (:u), which is what we want anyway.
#
# L1-dcache-* and LLC-* are NOT exposed under WSL2 (<not counted> / <not supported>),
# so the cache picture here is the generic cache-references/cache-misses pair and
# nothing finer. That is a real limit of this environment, not an omission.
set -u

PERF=$(ls -d /usr/lib/linux-tools/*/perf 2>/dev/null | head -1)
if [ -z "$PERF" ]; then echo "no versioned perf binary found" >&2; exit 2; fi

BIN=${1:-./build-bench/bench_profile}
REPS=${2:-3}
EVENTS=cycles,instructions,cache-references,cache-misses,branches,branch-misses
MODES="none rest trade cancel cancel_miss"

echo "perf:   $PERF"
echo "binary: $BIN"
echo "events: $EVENTS"
echo "reps:   $REPS (median reported)"
echo

for mode in $MODES; do
  for ev in $(echo "$EVENTS" | tr ',' ' '); do
    vals=""
    for _ in $(seq "$REPS"); do
      v=$("$PERF" stat -x, -e "$ev" "$BIN" "$mode" 2>&1 >/dev/null | head -1 | cut -d, -f1)
      case "$v" in ''|*[!0-9]*) v=0 ;; esac
      vals="$vals $v"
    done
    med=$(echo $vals | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
    echo "$mode,$ev,$med"
  done
done
