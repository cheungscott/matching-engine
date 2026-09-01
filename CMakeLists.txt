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

# -Wall -Wextra on OUR targets, propagated through the INTERFACE library so it
# reaches tests and benches but not Catch2's own translation units.
#
# Added after the fact, which is the point: an adversarial audit of the whole
# codebase missed a -Wreorder in OrderBook that this finds in one second. Not
# -Werror — a warning that breaks the build gets suppressed, and a suppressed
# warning is worse than an unread one. See D23.
if(NOT MSVC)
  target_compile_options(me_engine INTERFACE -Wall -Wextra)
endif()

# ---- Tests -----------------------------------------------------------------
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
