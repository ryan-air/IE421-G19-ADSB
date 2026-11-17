#include "adsb/common/clock.hpp"

#include <ctime>

namespace adsb {

namespace {

std::uint64_t read_clock(clockid_t id) noexcept {
  timespec ts{};
  clock_gettime(id, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// One (ticks, mono_ns) sample. The tick read is bracketed by two clock reads
// and attributed to their midpoint; the bracket width bounds the error.
struct Sample {
  std::uint64_t tick;
  std::uint64_t ns;
  std::uint64_t width;
};

Sample take_sample() noexcept {
  Sample best{0, 0, ~std::uint64_t{0}};
  // Keep the tightest of a few attempts to reject preemption / SMIs.
  for (int i = 0; i < 16; ++i) {
    const std::uint64_t a = mono_ns();
    const std::uint64_t t = ticks();
    const std::uint64_t b = mono_ns();
    if (b - a < best.width) best = {t, a + (b - a) / 2, b - a};
  }
  return best;
}

}  // namespace

std::uint64_t mono_ns() noexcept { return read_clock(CLOCK_MONOTONIC_RAW); }

std::uint64_t wall_ns() noexcept { return read_clock(CLOCK_REALTIME); }

std::uint64_t hardware_tick_hz() noexcept {
#if defined(__aarch64__)
  std::uint64_t hz;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
  return hz;
#else
  return 0;
#endif
}

TickConverter TickConverter::from_hz(std::uint64_t hz) noexcept {
  if (hz == 0) hz = 1;  // defensive: avoid division by zero
  const uint128 mult = ((static_cast<uint128>(1'000'000'000ULL) << kShift) + hz / 2) / hz;
  return TickConverter(static_cast<std::uint64_t>(mult), static_cast<double>(hz));
}

TickConverter TickConverter::calibrate(std::uint64_t duration_ns, bool force_measure) noexcept {
  if (!ticks_are_hardware()) return from_hz(1'000'000'000ULL);  // ticks() == mono_ns()
  if (!force_measure) {
    if (const std::uint64_t hz = hardware_tick_hz(); hz != 0) return from_hz(hz);
  }

  const Sample s0 = take_sample();
  timespec req{static_cast<time_t>(duration_ns / 1'000'000'000ULL),
               static_cast<long>(duration_ns % 1'000'000'000ULL)};
  while (nanosleep(&req, &req) != 0) {
    // EINTR: continue sleeping for the remainder.
  }
  const Sample s1 = take_sample();

  const std::uint64_t dt_ns = s1.ns - s0.ns;
  const std::uint64_t dticks = s1.tick - s0.tick;
  if (dt_ns == 0 || dticks == 0) return from_hz(1'000'000'000ULL);

  // hz = dticks * 1e9 / dt_ns, in 128-bit to avoid overflow.
  const uint128 hz = (static_cast<uint128>(dticks) * 1'000'000'000ULL + dt_ns / 2) / dt_ns;
  return from_hz(static_cast<std::uint64_t>(hz));
}

}  // namespace adsb
