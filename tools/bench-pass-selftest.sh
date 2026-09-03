#!/usr/bin/env bash
# tools/bench-pass-selftest.sh — plant a violation per branch of bench-pass.sh, and assert
# WHICH branch fired.
#
# WHY THIS EXISTS
#
# bench-pass.sh is a guard: code whose only job is to refuse, and which is never exercised
# in normal use. A guard branch that cannot fire looks exactly like one that never needed
# to, so each branch gets a planted violation here, and a dead or misordered check fails
# this script instead of staying silent.
#
# ORDER IS PART OF WHAT IS TESTED. A Debug build produces no bench targets, so if the
# binary-exists check ran before the build-type check, pointing at a Debug tree would
# always trip "no benchmark binary" and the "not Bench" message would be
# unreachable in the only case anyone would hit it. Test 2 catches that.
#
# EACH ASSERTION NAMES ITS BRANCH. Checking only "it exited non-zero" would let any branch
# satisfy any test, which is how a checker ends up passing for the wrong reason.
#
# PLANTS MUST BE DETERMINISTIC. The load violation is planted with ME_BENCH_MAX_LOAD=-1,
# which no load average can sit below. A ceiling of 0 would not do: an idle machine reads
# 0.00, and 0.00 > 0 is false, so the plant would pass or fail on ambient load. The guard
# would be right and the test flaky. A flaky plant is worse than no plant: it goes green by
# accident eventually, and gets believed.
#
# USAGE
#   tools/bench-pass-selftest.sh [-b BUILD_DIR] [-n RUNS_FOR_HAPPY_PATH]
#
# The happy path needs a real Bench build, so configure one first:
#   cmake --preset bench && cmake --build --preset bench
set -uo pipefail

BUILD_DIR=build-bench
HAPPY_RUNS=2

while [ $# -gt 0 ]; do
  case "$1" in
    -b) BUILD_DIR=$2; shift 2 ;;
    -n) HAPPY_RUNS=$2; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.." || exit 1
S=tools/bench-pass.sh
[ -f "$S" ] || { echo "cannot find $S" >&2; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0

# check NAME WANT_RC WANT_SUBSTRING GOT_RC OUTPUT
check() {
  local name=$1 want_rc=$2 want_str=$3 got_rc=$4 out=$5
  if [ "$got_rc" = "$want_rc" ] && printf '%s' "$out" | grep -qF -- "$want_str"; then
    echo "  PASS  $name  (exit $got_rc, matched \"$want_str\")"
    pass=$((pass+1))
  else
    echo "  FAIL  $name  (exit $got_rc, wanted $want_rc and \"$want_str\")"
    printf '%s\n' "$out" | sed 's/^/        | /' | head -8
    fail=$((fail+1))
  fi
}

# Synthetic trees rather than real build trees: these test the script's branching,
# not CMake, and a test that depends on which directories happen to exist is not a test.
mkdir -p "$TMP/debug-tree" "$TMP/bench-tree-no-binary"
printf 'CMAKE_BUILD_TYPE:STRING=Debug\n' > "$TMP/debug-tree/CMakeCache.txt"
printf 'CMAKE_BUILD_TYPE:STRING=Bench\n' > "$TMP/bench-tree-no-binary/CMakeCache.txt"

echo "=== 1. not a CMake build directory ==="
out=$(bash $S -b "$TMP/nonexistent" 2>&1); rc=$?
check "no-cmake-dir" 2 "is not a CMake build directory" "$rc" "$out"

echo "=== 2. a CMake tree, but not a Bench one ==="
out=$(bash $S -b "$TMP/debug-tree" 2>&1); rc=$?
check "wrong-build-type" 2 "not Bench" "$rc" "$out"

echo "=== 3. a Bench tree with no binary built ==="
out=$(bash $S -b "$TMP/bench-tree-no-binary" 2>&1); rc=$?
check "missing-binary" 2 "no benchmark binary at" "$rc" "$out"

echo "=== 4. the pre-run load guard refuses ==="
out=$(ME_BENCH_MAX_LOAD=-1 bash $S -b "$BUILD_DIR" -n 1 2>&1); rc=$?
check "pre-run-refuse" 1 "REFUSING: load average" "$rc" "$out"

echo "=== 5. force stamps the output, and the post-run check still fails the run ==="
out=$(ME_BENCH_MAX_LOAD=-1 ME_BENCH_FORCE=1 bash $S -b "$BUILD_DIR" -n 1 2>&1); rc=$?
check "force-stamp"    1 "FORCED: started at load"      "$rc" "$out"
check "post-run-drift" 1 "FAILED: load average rose to" "$rc" "$out"

echo "=== 6. happy path, ceiling raised so both guards pass ==="
if [ ! -x "$BUILD_DIR/bench_latency" ]; then
  # Not skipped quietly. A self-test that reports success having never exercised the success
  # path is the same defect it exists to catch.
  echo "  FAIL  happy-path  (no binary at $BUILD_DIR/bench_latency)"
  echo "        build it first:  cmake --preset bench && cmake --build --preset bench"
  fail=$((fail+1))
else
  out=$(ME_BENCH_MAX_LOAD=99 bash $S -b "$BUILD_DIR" -n "$HAPPY_RUNS" 2>&1); rc=$?
  check "happy-exit"  0 "QUOTE THESE AS BANDS" "$rc" "$out"
  check "happy-bands" 0 "p99.9 ns"             "$rc" "$out"
  printf '%s\n' "$out" | grep -A4 "QUOTE THESE AS BANDS" | sed 's/^/        /'
  echo "        (a ceiling of 99 defeats the guard on purpose — these numbers are"
  echo "         a parser check, NOT a measurement. Use tools/bench-pass.sh for that.)"
fi

echo
echo "planted branches: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
