#include "adsb/common/clock.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace {

TEST(Clock, MonoIsNonDecreasing) {
  std::uint64_t prev = adsb::mono_ns();
  for (int i = 0; i < 100'000; ++i) {
    const std::uint64_t now = adsb::mono_ns();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(Clock, TicksAreNonDecreasingOnOneThread) {
  std::uint64_t prev = adsb::ticks();
  for (int i = 0; i < 100'000; ++i) {
    const std::uint64_t now = adsb::ticks();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(Clock, TickDeltaClampsReorderedReads) {
  // A consumer's counter read can land a few ticks before the producer's
  // (unordered counter reads): that must count as 0, not wrap to ~2^64.
  EXPECT_EQ(adsb::tick_delta(100, 40), 60u);
  EXPECT_EQ(adsb::tick_delta(40, 40), 0u);
  EXPECT_EQ(adsb::tick_delta(39, 40), 0u);
  EXPECT_EQ(adsb::tick_delta(0, ~std::uint64_t{0}), 0u);
}

TEST(Clock, WallClockIsPlausible) {
  // After 2020-01-01 and before 2100-01-01.
  const std::uint64_t now = adsb::wall_ns();
  EXPECT_GT(now, 1'577'836'800ULL * 1'000'000'000ULL);
  EXPECT_LT(now, 4'102'444'800ULL * 1'000'000'000ULL);
}

TEST(TickConverter, FromHzIsExactForSimpleRates) {
  const auto ghz = adsb::TickConverter::from_hz(1'000'000'000ULL);
  EXPECT_EQ(ghz.to_ns(0), 0u);
  EXPECT_EQ(ghz.to_ns(12345), 12345u);

  const auto mhz24 = adsb::TickConverter::from_hz(24'000'000ULL);  // Apple silicon
  EXPECT_NEAR(static_cast<double>(mhz24.to_ns(24'000'000ULL)), 1e9, 1.0);

  const auto ghz3 = adsb::TickConverter::from_hz(3'000'000'000ULL);  // typical TSC
  EXPECT_NEAR(static_cast<double>(ghz3.to_ns(3'000'000'000ULL)), 1e9, 1.0);
  // Large deltas (an hour of TSC ticks) must not overflow.
  EXPECT_NEAR(static_cast<double>(ghz3.to_ns(3'000'000'000ULL * 3600)), 3600e9, 3600.0);
}

// Checks a converter against mono_ns over a real interval.
void expect_tracks_mono(const adsb::TickConverter& conv) {
  const std::uint64_t t0 = adsb::mono_ns();
  const std::uint64_t k0 = adsb::ticks();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const std::uint64_t k1 = adsb::ticks();
  const std::uint64_t t1 = adsb::mono_ns();
  const double measured = static_cast<double>(conv.to_ns(k1 - k0));
  const double reference = static_cast<double>(t1 - t0);
  // 1% tolerance is generous for a 100 ms window; it catches unit errors
  // (wrong frequency, wrong shift) rather than measuring clock quality.
  EXPECT_NEAR(measured, reference, 0.01 * reference)
      << "ticks/s=" << conv.ticks_per_second();
}

TEST(TickConverter, DefaultCalibrationTracksMonotonicClock) {
  const auto conv = adsb::TickConverter::calibrate();
  EXPECT_GT(conv.ticks_per_second(), 1e6);
  expect_tracks_mono(conv);
}

TEST(TickConverter, MeasuredCalibrationTracksMonotonicClock) {
  const auto conv = adsb::TickConverter::calibrate(50'000'000, /*force_measure=*/true);
  EXPECT_GT(conv.ticks_per_second(), 1e6);
  expect_tracks_mono(conv);
  if (const std::uint64_t hw = adsb::hardware_tick_hz(); hw != 0) {
    // Measured and architectural frequency should agree closely.
    EXPECT_NEAR(conv.ticks_per_second(), static_cast<double>(hw), 0.005 * static_cast<double>(hw));
  }
}

}  // namespace
