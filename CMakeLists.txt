# Matching Engine — build config.
#
# THREE configurations, and confusing them is how you end up with a CV number
# you cannot reproduce:
#
#   Debug   -O0 -g  + ASan/UBSan   the entire test suite lives here (Phases 1-7)
#   TSan    -O1 -g  + TSan         the ring buffer only (Phases 8-9). Cannot coexist with ASan.
#   Bench   -O2/-O3, NO sanitizers the ONLY build allowed to produce a latency number
#
#   cmake -S . -B build       -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   cmake -S . -B build-tsan  -G Ninja -DCMAKE_BUILD_TYPE=TSan
#   cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Bench
#   cmake --build build && ctest --test-dir build --output-on-failure
#
# NOTE: MinGW ships no libasan and no libtsan, and TSan has never been ported to
# Windows by any vendor. Sanitized builds require WSL/Linux. See the vault's
# Matching-Engine-Toolchain-Decision.md.
#
# NOTE: the test framework is Catch2 v3, fetched from GitHub at configure time.
# The first configure needs network and takes a minute; after that it is cached
# in the build directory. There is no longer a "g++ alone, no cmake" path.
cmake_minimum_required(VERSION 3.22)   # WSL ships 3.22.1; nothing here needs 3.24
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Debug | TSan | Bench" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug TSan Bench)

# ---- Sanitizers are tied to the build type, never defaulted ON globally -----
# The old `option(ME_ASAN ... ON)` applied sanitizers to EVERY build including a
# benchmark one. ASan costs ~2x; a latency number measured under it is inflated
# and unreproducible, which voids the whole §8 measurement discipline.
#
# These are set BEFORE Catch2 is fetched, deliberately: a sanitized binary must
# have every translation unit built and linked with the same sanitizer flags.
if(NOT MSVC)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  elseif(CMAKE_BUILD_TYPE STREQUAL "TSan")
    add_compile_options(-O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  elseif(CMAKE_BUILD_TYPE STREQUAL "Bench")
    add_compile_options(-O2 -g -DNDEBUG)
  else()
    # D27/R16. STREQUAL is case-sensitive and there was no else branch, so
    # -DCMAKE_BUILD_TYPE=bench or =Release or an empty value silently produced NO
    # optimisation, NO -g, NO NDEBUG, NO sanitizers and no bench targets — a build
    # that looks like every other build and verifies and measures nothing. Refuse.
    message(FATAL_ERROR
      "CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'. This project defines exactly three, "
      "and they are case-sensitive: Debug (ASan+UBSan, the one that verifies), TSan "
      "(for the ring buffer when it arrives), Bench (-O2 -DNDEBUG, the ONLY one "
      "permitted to produce a latency number). Anything else gets no flags at all.")
  endif()
endif()

# Loud refusal rather than a silently unsanitized "Debug" build.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32 AND NOT MSVC)
  message(WARNING
    "Debug on MinGW has NO AddressSanitizer (no libasan for this target). "
    "Phase 1's acceptance criterion is 'ASan clean' — build Debug under WSL/Linux "
    "for that. This build will compile and run, but it verifies less than it looks.")
endif()

# ---- Test framework --------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2      # pinned: a floating tag makes builds unreproducible
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ---- Warnings --------------------------------------------------------------
# Deliberately AFTER FetchContent: directory-scope options only reach targets
# created later, so this warns on OUR code without drowning the output in
# warnings from someone else's library that we are not going to fix.
if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wconversion)
endif()

add_library(me_engine INTERFACE)
target_include_directories(me_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# NOTE: warnings are set at directory scope above (-Wall -Wextra -Wconversion), which
# already reaches every target declared after it and is strictly stronger than what
# used to be repeated here as INTERFACE options on me_engine. That duplicate was dead
# and has been removed (D27/R17) — the comment describing it as the mechanism was the
# misleading part, since a reader would have looked in the wrong place to change it.
#
# Not -Werror: a warning that breaks the build gets suppressed, and a suppressed
# warning is worse than an unread one. See D23.

# ---- Tests -----------------------------------------------------------------
# D27/1.21. These were unconditional, so `ctest --test-dir build-bench` ran the whole
# suite green under -O2 -DNDEBUG with no sanitizers and every assert compiled out —
# verifying far less than it appeared to, with nothing saying so. Bench builds
# benchmarks and nothing else now.
if(CMAKE_BUILD_TYPE STREQUAL "Bench")
  message(STATUS "Bench build: tests are NOT built. Verification lives in the Debug build.")
else()

enable_testing()
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# Phase 0: value-type invariants.
add_executable(test_types tests/test_types.cpp)
target_link_libraries(test_types PRIVATE me_engine Catch2::Catch2WithMain)
catch_discover_tests(test_types)

# Phase 1 acceptance suite. RED until the include/me/*.hpp bodies are written.
add_executable(phase1_tests tests/phase1_tests.cpp)
target_link_libraries(phase1_tests PRIVATE me_engine Catch2::Catch2WithMain)
target_include_directories(phase1_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
catch_discover_tests(phase1_tests)

# D27/1.1. catch_discover_tests does not enumerate hidden [.gate] tests, so the three
# most expensive cases in the project — the million-operation property run, the 100k
# differential with invariants, and the shrinker — were INVISIBLE to `ctest`, which is
# the command this file and the README both document. Following the instructions gave
# a green run with the entire Phase 7 gate omitted.
#
# Registered explicitly, and it runs BY DEFAULT: the safe thing should happen unless
# you opt out. For the fast edit loop, opt out by label:
#     ctest --test-dir build -LE gate
add_test(NAME phase7_gate COMMAND phase1_tests "[gate]")
set_tests_properties(phase7_gate PROPERTIES LABELS "gate" TIMEOUT 2400)

endif()   # NOT Bench

# ---- Benchmarks ------------------------------------------------------------
# Bench ONLY. The binary itself also refuses to run under a sanitizer or with
# assertions enabled, so a wrong number cannot be produced by accident from
# either direction (D17).
if(CMAKE_BUILD_TYPE STREQUAL "Bench")
  add_executable(bench_latency bench/latency.cpp)
  target_link_libraries(bench_latency PRIVATE me_engine)

  # One operation kind in a tight loop, for perf to sample (D19).
  add_executable(bench_profile bench/profile.cpp)
  target_link_libraries(bench_profile PRIVATE me_engine)

  # The oracle as a performance baseline, which D6 promised and never built (D24).
  # Reaches into tests/ for naive_book.hpp deliberately: one strawman, used by both
  # the correctness suite and the measurement, so they cannot drift apart.
  add_executable(bench_baseline bench/baseline.cpp)
  target_link_libraries(bench_baseline PRIVATE me_engine)
  target_include_directories(bench_baseline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
