#pragma once

// Clocks for latency measurement.
//
//  * mono_ns()      CLOCK_MONOTONIC_RAW in nanoseconds. Not NTP-slewed, so it
//                   is the reference for durations. ~20-50 ns per call (vDSO).
//  * ticks()        Raw hardware counter: TSC on x86-64, CNTVCT_EL0 on AArch64.
//                   A few ns per call, no syscall. NOT CPU cycles on ARM: the
//                   generic timer runs at a fixed rate (24 MHz on Apple
//                   silicon, 54 MHz on Raspberry Pi 5), so resolution there is
//                   ~18-42 ns. On x86 the TSC is only trustworthy if the CPU
//                   reports constant_tsc + nonstop_tsc (see scripts/check-host.sh).
//  * TickConverter  Converts tick deltas to nanoseconds with a fixed-point
//                   multiply/shift (no floating point, no division on the hot
//                   path), calibrated against mono_ns().

#include <cstdint>

#include "adsb/common/int128.hpp"

namespace adsb {

// CLOCK_MONOTONIC_RAW, in nanoseconds.
[[nodiscard]] std::uint64_t mono_ns() noexcept;

// CLOCK_REALTIME, in nanoseconds since the Unix epoch. Use only for labelling
// results or cross-host comparison under chrony/PTP, never for durations.
[[nodiscard]] std::uint64_t wall_ns() noexcept;

// Reads the hardware counter. Not serializing: surrounding instructions may be
// reordered around it, which is the right trade-off for stage timestamps.
[[nodiscard]] inline std::uint64_t ticks() noexcept {
#if defined(__x86_64__)
  return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
  std::uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  return mono_ns();
#endif
}

// Tick rate reported by the hardware, or 0 if it must be measured.
// AArch64 exposes it in CNTFRQ_EL0; x86 has no reliable architectural source.
[[nodiscard]] std::uint64_t hardware_tick_hz() noexcept;

// True when ticks() is a real hardware counter (false on the mono_ns fallback).
[[nodiscard]] constexpr bool ticks_are_hardware() noexcept {
#if defined(__x86_64__) || defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

// Ticks elapsed from `earlier` to `later`, clamped at 0. Counter reads are
// not ordered against memory accesses (no ISB / LFENCE, by design), so a
// timestamp taken just after popping an item from another thread can read a
// few ns *before* the producer's timestamp for that item. Subtracting raw
// would wrap to ~2^64 ticks and poison the histogram's max (seen in
// adsb-replay's queue latency). Use this for any cross-thread interval.
[[nodiscard]] constexpr std::uint64_t tick_delta(std::uint64_t later, std::uint64_t earlier) noexcept {
  return later > earlier ? later - earlier : 0;
}

class TickConverter {
 public:
  // Calibrates by sampling ticks() and mono_ns() over `duration_ns`.
  // On AArch64 the architectural frequency is used directly unless
  // `force_measure` is set (useful for testing the measurement path).
  static TickConverter calibrate(std::uint64_t duration_ns = 50'000'000,
                                 bool force_measure = false) noexcept;

  // Builds a converter for a known frequency.
  static TickConverter from_hz(std::uint64_t hz) noexcept;

  [[nodiscard]] std::uint64_t to_ns(std::uint64_t tick_delta) const noexcept {
    return static_cast<std::uint64_t>(
        (static_cast<uint128>(tick_delta) * mult_) >> kShift);
  }

  [[nodiscard]] double ticks_per_second() const noexcept { return hz_; }

 private:
  // 32 fractional bits. mult is rounded to nearest, so the conversion error is
  // at most 0.5 / mult: < 0.2 ppb for tick rates up to ~4 GHz (the TSC) and far
  // smaller for the 24-54 MHz ARM timers. The 128-bit product cannot overflow
  // for any 64-bit delta.
  static constexpr unsigned kShift = 32;

  TickConverter(std::uint64_t mult, double hz) noexcept : mult_(mult), hz_(hz) {}

  std::uint64_t mult_;  // ns per tick, scaled by 2^kShift
  double hz_;
};

}  // namespace adsb
