# Matching Engine — build config.
#
# THREE configurations, and confusing them is how you end up with a latency number
# you cannot reproduce:
#
#   Debug   -O0 -g  + ASan/UBSan   the entire test suite lives here
#   TSan    -O1 -g  + TSan         reserved for concurrent code. Cannot coexist with ASan.
#   Bench   -O2/-O3, NO sanitizers the ONLY build allowed to produce a latency number
#
#   cmake -S . -B build       -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   cmake -S . -B build-tsan  -G Ninja -DCMAKE_BUILD_TYPE=TSan
#   cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Bench
#   cmake --build build && ctest --test-dir build --output-on-failure
#
# NOTE: MinGW ships no libasan and no libtsan, and TSan has never been ported to
# Windows by any vendor. Sanitized builds require WSL/Linux.
#
# NOTE: the test framework is Catch2 v3, fetched from GitHub at configure time.
# The first configure needs network and takes a minute; after that it is cached
# in the build directory. Building without CMake is not supported.
cmake_minimum_required(VERSION 3.22)   # Ubuntu 22.04 ships 3.22.1; nothing here needs 3.24
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# `std::expected` on the reject path needs libstdc++ 12. Under 11.4 the failure
# is several screens of template errors that never name the cause, so refuse early and
# say what to do instead.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  message(FATAL_ERROR
    "g++ ${CMAKE_CXX_COMPILER_VERSION} cannot build this project: std::expected needs g++ 12 "
    "or newer. Ubuntu 22.04 defaults to 11.4 - run `sudo apt install g++-12`, then configure "
    "with -DCMAKE_CXX_COMPILER=g++-12.")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Debug | TSan | Bench" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug TSan Bench)

# ---- Sanitizers are tied to the build type, never defaulted ON globally -----
# A global sanitizer option would apply ASan to EVERY build, including a
# benchmark one. ASan costs ~2x; a latency number measured under it is inflated
# and unreproducible, which voids the whole measurement discipline.
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
    # STREQUAL is case-sensitive, so without this branch -DCMAKE_BUILD_TYPE=bench or
    # =Release would silently produce NO optimisation, NO -g, NO NDEBUG, NO sanitizers
    # and no bench targets: a build that looks like every other build and verifies and
    # measures nothing. Refuse.
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
    "The test suite's acceptance criterion is 'ASan clean' — build Debug under WSL/Linux "
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
# created later, so this warns on the project's own code without drowning the output
# in warnings from a third-party library that will not be fixed here.
if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wconversion)
endif()

add_library(me_engine INTERFACE)
target_include_directories(me_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# NOTE: warnings are set at directory scope above (-Wall -Wextra -Wconversion), which
# reaches every target declared after it, so me_engine carries no INTERFACE warning
# options of its own. Change warnings there, not here.
#
# Not -Werror: a warning that breaks the build gets suppressed, and a suppressed
# warning is worse than an unread one.

# ---- Tests -----------------------------------------------------------------
# Not built under Bench. There, `ctest --test-dir build-bench` would run the whole
# suite green under -O2 -DNDEBUG with no sanitizers and every assert compiled out,
# verifying far less than it appears to, with nothing saying so. Bench builds
# benchmarks and nothing else.
if(CMAKE_BUILD_TYPE STREQUAL "Bench")
  message(STATUS "Bench build: tests are NOT built. Verification lives in the Debug build.")
else()

enable_testing()
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# Value-type invariants.
add_executable(test_types tests/test_types.cpp)
target_link_libraries(test_types PRIVATE me_engine Catch2::Catch2WithMain)
catch_discover_tests(test_types)

# Engine acceptance suite.
add_executable(phase1_tests tests/phase1_tests.cpp)
target_link_libraries(phase1_tests PRIVATE me_engine Catch2::Catch2WithMain)
target_include_directories(phase1_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
catch_discover_tests(phase1_tests)

# catch_discover_tests does not enumerate hidden [.gate] tests, so the three
# most expensive cases in the project (the million-operation property run, the 100k
# differential with invariants, and the shrinker) would be INVISIBLE to `ctest`
# without this, and a green run would omit the entire fuzz gate.
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
# either direction.
if(CMAKE_BUILD_TYPE STREQUAL "Bench")
  add_executable(bench_latency bench/latency.cpp)
  target_link_libraries(bench_latency PRIVATE me_engine)

  # One operation kind in a tight loop, for perf to sample.
  add_executable(bench_profile bench/profile.cpp)
  target_link_libraries(bench_profile PRIVATE me_engine)

  # The oracle as a performance baseline.
  # Reaches into tests/ for naive_book.hpp deliberately: one strawman, used by both
  # the correctness suite and the measurement, so they cannot drift apart.
  add_executable(bench_baseline bench/baseline.cpp)
  target_link_libraries(bench_baseline PRIVATE me_engine)
  target_include_directories(bench_baseline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
