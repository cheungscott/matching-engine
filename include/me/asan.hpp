// me/asan.hpp — portable AddressSanitizer poisoning shim.
//
// WHY THIS EXISTS (the finding that motivated it):
//
// ASan does NOT police object-pool discipline by default. Your pool allocates
// one big slab up front; to ASan that slab is a single live allocation for the
// program's lifetime, so every byte in it is legal forever. `release()` putting
// a slot back on the free-list involves no free(), so use-after-release inside
// the pool — Blueprint invariant 7, the exact bug this project will produce — is
// INVISIBLE unless you poison manually.
//
// Demonstrated: identical use-after-free was caught when the memory came from
// `new`, and silently returned stale data when it came from a slab pool.
//
// TWO RULES, both learned the hard way:
//
//   1. Poison whole ALIGNED 8-BYTE GRANULES. ASan's shadow memory maps 8 bytes
//      of real memory to 1 shadow byte, so "bytes 0-3 dead, bytes 4-7 alive" is
//      not representable and a sub-granule poison is SILENTLY IGNORED. A 4-byte
//      poison did nothing; an 8-byte aligned one fired correctly.
//
//   2. Keep the free-list link OUTSIDE the poisoned region (or unpoison before
//      traversing), or walking your own free-list trips the sanitizer.
//
// Rule 2 is a constraint on your Order layout, so decide it before you write
// the pool, not after.
#pragma once

#include <cstddef>

// __SANITIZE_ADDRESS__ is GCC; __has_feature(address_sanitizer) is Clang.
// Neither is defined on MinGW, which ships no libasan at all — the macros below
// then compile to nothing and the pool still works, just unverified.
#if defined(__SANITIZE_ADDRESS__)
#  define ME_HAS_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ME_HAS_ASAN 1
#  endif
#endif

#if defined(ME_HAS_ASAN)
#  include <sanitizer/asan_interface.h>
#  define ME_POISON(addr, size)   __asan_poison_memory_region((addr), (size))
#  define ME_UNPOISON(addr, size) __asan_unpoison_memory_region((addr), (size))
#else
#  define ME_HAS_ASAN 0
#  define ME_POISON(addr, size)   ((void)(addr), (void)(size))
#  define ME_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif

namespace me {

// Round a size DOWN to a whole number of ASan shadow granules. Poisoning
// `granule_floor(n)` bytes is always representable; poisoning `n` may not be.
constexpr std::size_t kAsanGranule = 8;
constexpr std::size_t granule_floor(std::size_t n) noexcept {
    return n - (n % kAsanGranule);
}

} // namespace me
