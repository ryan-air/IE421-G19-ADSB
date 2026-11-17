#pragma once

#include <cstddef>

namespace adsb {

// Alignment used to keep independently written data on separate cache lines.
//
// 128 bytes rather than 64 on purpose:
//  * Apple M-series cores have 128-byte L1 lines.
//  * Intel's L2 "adjacent line" prefetcher pulls 64-byte lines in pairs, so two
//    variables 64 bytes apart can still false-share on x86.
// std::hardware_destructive_interference_size is not used because its value is
// compiler-dependent (and GCC warns that it is ABI-unstable).
inline constexpr std::size_t kCacheLine = 128;

// Hint to the CPU that we are in a spin-wait loop. Reduces power and, on SMT
// cores, gives execution resources to the sibling thread.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  // No portable pause instruction; fall through (pure busy-spin).
#endif
}

}  // namespace adsb
