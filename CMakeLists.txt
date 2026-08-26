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
# No CMake yet? The whole Phase 1 suite builds with g++ alone:
#   g++ -std=c++23 -Wall -Wextra -Wconversion -Iinclude tests/phase1_tests.cpp -o p1 && ./p1
#
# NOTE: MinGW ships no libasan and no libtsan, and TSan has never been ported to
# Windows by any vendor. Sanitized builds require WSL/Linux. See the vault's
# Matching-Engine-Toolchain-Decision.md.
cmake_minimum_required(VERSION 3.24)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Debug | TSan | Bench" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug TSan Bench)

# Warnings-as-signal. Add -Werror once the codebase settles.
if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wconversion)
endif()

# ---- Sanitizers are tied to the build type, never defaulted ON globally -----
# The old `option(ME_ASAN ... ON)` applied sanitizers to EVERY build including a
# benchmark one. ASan costs ~2x; a latency number measured under it is inflated
# and unreproducible, which voids the whole §8 measurement discipline.
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

add_library(me_engine INTERFACE)
target_include_directories(me_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- Tests -----------------------------------------------------------------
enable_testing()

# Phase 1 suite: framework-free so it builds with g++ alone, no network, no CMake.
# Retire it in favour of Catch2 once cmake is installed and FetchContent can run.
add_executable(phase1_tests tests/phase1_tests.cpp)
target_link_libraries(phase1_tests PRIVATE me_engine)
add_test(NAME phase1 COMMAND phase1_tests)

# Benchmarks never link a sanitizer; guard against being built in Debug by mistake.
if(CMAKE_BUILD_TYPE STREQUAL "Bench")
  # add_executable(bench bench/latency.cpp)   # Phase 10
  # target_link_libraries(bench PRIVATE me_engine)
endif()
