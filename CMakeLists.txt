# Matching Engine — build config (Phase 0).
#
# This is the intended build path once you install CMake (winget install Kitware.CMake).
# Until then, the framework-free smoke test builds with g++ alone:
#   g++ -std=c++23 -Wall -Wextra -Wconversion -Iinclude tools/smoke.cpp -o smoke && ./smoke
#
# With CMake:
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.24)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings-as-signal. Add -Werror once the codebase settles.
if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wconversion)
endif()

# Sanitizers: ON by default for the debug/test build. The intrusive-list +
# object-pool work (Phase 3+) makes ASan/UBSan essential — they catch dangling
# Order* and double-free the day you write them. TSan replaces ASan for the ring
# buffer's tests (Phase 8) — you cannot run both at once.
option(ME_ASAN "Enable AddressSanitizer + UBSan" ON)
if(ME_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(me_engine INTERFACE)             # header-only for now (types.hpp)
target_include_directories(me_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- Tests (Catch2 via FetchContent) --------------------------------------
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

# RapidCheck (property-based tests) arrives in earnest at Phase 2/7. Declared
# now so the wiring is ready; uncomment when you write the first property test.
# FetchContent_Declare(
#   rapidcheck
#   GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
#   GIT_TAG        master
# )
# FetchContent_MakeAvailable(rapidcheck)

enable_testing()
add_executable(unit_tests tests/test_types.cpp)
target_link_libraries(unit_tests PRIVATE me_engine Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(unit_tests)
