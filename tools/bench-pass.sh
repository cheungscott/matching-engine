#!/usr/bin/env bash
# tools/bench-pass.sh — the latency pass, reproducibly, and only on an idle machine.
#
# This is tools/perf-pass.sh's argument applied to latency. That file exists because numbers
# "quoted from a terminal nobody still has open" are not evidence. The same is true of numbers
# quoted from a terminal nobody checked the load average of. See D32.
#
# WHY IT REFUSES RATHER THAN WARNS
#
# The published tail has now been called irreproducible TWICE, and both times the machine was
# busy: once compiling and running the ASan fuzz gate, once with a WSL distro mid-boot at load
# 0.92. Both readings were wrong. A warning printed above a table does not survive the table
# being copied somewhere else, so this refuses instead — the same call D17 made when it had the
# benchmark binaries refuse to run under a sanitizer or without NDEBUG. A wrong number should
# take effort to produce.
#
# THE POST-RUN CHECK IS THE HALF THAT MATTERS
#
# Load average is a one-minute exponential average, so it lags: it can read clean and then
# degrade mid-run, which is close to exactly what happened on 2026-09-03 (0.27 rising to 0.92).
# So the load is checked again AFTER the invocations, and a run that drifted FAILS rather than
# mentioning it in passing.
#
# WHAT IT DELIBERATELY DOES NOT DO
#
# It does not compare against canonical figures or emit a pass/fail verdict on performance.
# That would be a performance gate on an unpinned virtual machine — the thing D30 kept out of
# CI because it would flap. This reports; you judge.
#
# USAGE
#   tools/bench-pass.sh [-n RUNS] [-b BUILD_DIR]
#
#   ME_BENCH_MAX_LOAD=0.20   one-minute loadavg ceiling (default 0.20)
#   ME_BENCH_FORCE=1         run anyway; the output is STAMPED as forced so the number
#                            cannot later be mistaken for a clean one
set -uo pipefail

RUNS=10
BUILD_DIR=build-bench
MAX_LOAD=${ME_BENCH_MAX_LOAD:-0.20}
FORCE=${ME_BENCH_FORCE:-0}

while [ $# -gt 0 ]; do
  case "$1" in
    -n) RUNS=$2; shift 2 ;;
    -b) BUILD_DIR=$2; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.." || exit 1
BIN="$BUILD_DIR/bench_latency"
CACHE="$BUILD_DIR/CMakeCache.txt"

# Order matters, and it was wrong first time round: checking for the binary before checking
# the build type made the build-type branch UNREACHABLE, because a Debug tree builds no bench
# targets at all, so pointing at one always tripped the missing-binary message instead. Found
# by planting a violation per branch and asserting which one fired. Identify the tree first,
# then complain about what is missing from it.
if [ ! -f "$CACHE" ]; then
  echo "$BUILD_DIR is not a CMake build directory (no CMakeCache.txt)." >&2
  echo "configure it first:  cmake --preset bench" >&2
  exit 2
fi

cache_get() { grep -m1 "^$1:" "$CACHE" 2>/dev/null | cut -d= -f2; }
BUILD_TYPE=$(cache_get CMAKE_BUILD_TYPE)
COMPILER=$(cache_get CMAKE_CXX_COMPILER)
GENERATOR=$(cache_get CMAKE_GENERATOR)

# The binary refuses sanitized and assert-enabled builds itself (D17); this only catches the
# case where you pointed the script at the wrong tree entirely.
if [ "$BUILD_TYPE" != "Bench" ]; then
  echo "$BUILD_DIR is a '$BUILD_TYPE' build, not Bench. Only Bench may produce a latency figure." >&2
  exit 2
fi

if [ ! -x "$BIN" ]; then
  echo "no benchmark binary at $BIN" >&2
  echo "build it first:  cmake --preset bench && cmake --build --preset bench" >&2
  exit 2
fi

load1() { cut -d' ' -f1 /proc/loadavg; }
over() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>b)}'; }

LOAD_BEFORE=$(load1)
echo "=== matching-engine latency pass ==="
echo "binary      $BIN"
echo "build       $BUILD_TYPE, $GENERATOR, $COMPILER"
echo "runs        $RUNS invocations (each itself a median of 5 in-process runs)"
echo "cores       $(nproc)"
echo "kernel      $(uname -r)"
echo "load before $(cat /proc/loadavg)"
echo "ceiling     $MAX_LOAD (one-minute average)"
echo

if over "$LOAD_BEFORE" "$MAX_LOAD"; then
  if [ "$FORCE" != "1" ]; then
    echo "REFUSING: load average $LOAD_BEFORE exceeds $MAX_LOAD." >&2
    echo "The published tail has been called irreproducible twice, and both times the machine" >&2
    echo "was busy. Wait for the machine to settle, or raise ME_BENCH_MAX_LOAD deliberately." >&2
    echo "ME_BENCH_FORCE=1 overrides and stamps the output as forced." >&2
    exit 1
  fi
  echo "!!! FORCED: started at load $LOAD_BEFORE, above the $MAX_LOAD ceiling."
  echo "!!! These numbers are NOT clean-machine numbers. Do not publish them."
  echo
fi

printf '%-5s %8s %8s %12s\n' run p50 p99.9 'M ops/sec'
p50s=""; tails=""; thrus=""
for i in $(seq 1 "$RUNS"); do
  out=$("$BIN" 2>&1) || { echo "invocation $i failed:" >&2; echo "$out" >&2; exit 1; }
  all=$(printf '%s\n' "$out" | grep -m1 '^  ALL')
  p50=$(printf  '%s\n' "$all" | sed -E 's/.*p50= *([0-9]+).*/\1/')
  t999=$(printf '%s\n' "$all" | sed -E 's/.*p99\.9= *([0-9]+).*/\1/')
  thru=$(printf '%s\n' "$out" | grep -m1 'QUOTE THIS' | sed -E 's/^ *([0-9.]+) M ops.*/\1/')
  printf '%-5s %8s %8s %12s\n' "$i" "$p50" "$t999" "$thru"
  p50s="$p50s $p50"; tails="$tails $t999"; thrus="$thrus $thru"
done

LOAD_AFTER=$(load1)
echo
echo "load after  $(cat /proc/loadavg)"
echo

band() {
  printf '%s\n' "$2" | tr ' ' '\n' | grep -v '^$' | sort -g | awk -v l="$1" '
    {a[NR]=$1}
    END {
      med = (NR%2) ? a[int((NR+1)/2)] : (a[NR/2]+a[NR/2+1])/2
      printf "  %-11s %s to %s, median %s   (n=%d)\n", l, a[1], a[NR], med, NR
    }'
}
echo "QUOTE THESE AS BANDS WITH THE RUN COUNT, NEVER AS SINGLE FIGURES:"
band "p50 ns"      "$p50s"
band "p99.9 ns"    "$tails"
band "M ops/sec"   "$thrus"
echo
echo "  Environment must travel with any figure above: $(uname -r), $(nproc) cores,"
echo "  $COMPILER. If this is WSL2 it is a guest, an isolated pinned core is not"
echo "  achievable from inside one, and the far tail is contaminated by the hypervisor."

# The lagging-average catch. This is the check that would have caught 2026-09-03.
if over "$LOAD_AFTER" "$MAX_LOAD"; then
  echo
  echo "FAILED: load average rose to $LOAD_AFTER during the run, above the $MAX_LOAD ceiling." >&2
  echo "It read $LOAD_BEFORE at the start, so the ceiling passed and the machine got busy anyway." >&2
  echo "DISCARD these numbers. Loadavg is a one-minute average and lags; that is why this is" >&2
  echo "checked at both ends." >&2
  exit 1
fi

if [ "$FORCE" = "1" ]; then
  echo
  echo "!!! FORCED RUN. Not clean-machine numbers." >&2
  exit 1
fi
