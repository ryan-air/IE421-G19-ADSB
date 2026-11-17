#include "adsb/common/histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace {

using Hist = adsb::LogLinearHistogram<10, 40>;

// Exact nearest-rank quantile of a sorted sample.
std::uint64_t exact_quantile(const std::vector<std::uint64_t>& sorted, double q) {
  if (q <= 0.0) return sorted.front();
  auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size())));
  rank = std::clamp<std::size_t>(rank, 1, sorted.size());
  return sorted[rank - 1];
}

TEST(Histogram, EmptyIsZero) {
  auto h = std::make_unique<Hist>();
  EXPECT_EQ(h->count(), 0u);
  EXPECT_EQ(h->min(), 0u);
  EXPECT_EQ(h->max(), 0u);
  EXPECT_EQ(h->percentile(50), 0u);
  EXPECT_EQ(h->mean(), 0.0);
}

TEST(Histogram, BucketIndexIsContiguousAndMonotonic) {
  // Every bucket's [low, high] range must tile the value space with no gaps or
  // overlaps, and index_of must map both ends back to the same bucket.
  std::uint64_t expected_low = 0;
  for (std::size_t i = 0; i < Hist::kBucketCount; ++i) {
    const std::uint64_t lo = Hist::bucket_low(i);
    const std::uint64_t hi = Hist::bucket_high(i);
    ASSERT_EQ(lo, expected_low) << "gap or overlap at bucket " << i;
    ASSERT_LE(lo, hi);
    ASSERT_EQ(Hist::index_of(lo), i);
    ASSERT_EQ(Hist::index_of(hi), i);
    expected_low = hi + 1;
  }
  EXPECT_EQ(expected_low - 1, Hist::kMaxTrackable) << "buckets must end exactly at the max";
}

TEST(Histogram, SmallValuesAreExact) {
  for (std::uint64_t v = 0; v < (1u << 10); ++v) {
    EXPECT_EQ(Hist::bucket_low(Hist::index_of(v)), v);
    EXPECT_EQ(Hist::bucket_high(Hist::index_of(v)), v);
  }
}

TEST(Histogram, BucketWidthRespectsRelativeErrorBound) {
  for (std::size_t i = 0; i < Hist::kBucketCount; ++i) {
    const double lo = static_cast<double>(Hist::bucket_low(i));
    const double hi = static_cast<double>(Hist::bucket_high(i));
    if (lo == 0) continue;
    ASSERT_LE((hi - lo) / lo, Hist::kMaxRelativeError) << "bucket " << i;
  }
}

TEST(Histogram, PercentilesMatchExactWithinErrorBound) {
  auto h = std::make_unique<Hist>();
  std::mt19937_64 rng(42);
  // Log-normal-ish latencies spanning ~100 ns to ~10 ms, plus a few outliers.
  std::lognormal_distribution<double> dist(std::log(2'000.0), 1.2);
  std::vector<std::uint64_t> values;
  for (int i = 0; i < 200'000; ++i) {
    const auto v = static_cast<std::uint64_t>(dist(rng)) + 1;
    values.push_back(v);
    h->record(v);
  }
  for (const std::uint64_t outlier : {5'000'000'000ULL, 123'456'789ULL}) {
    values.push_back(outlier);
    h->record(outlier);
  }
  std::sort(values.begin(), values.end());

  EXPECT_EQ(h->count(), values.size());
  EXPECT_EQ(h->min(), values.front());
  EXPECT_EQ(h->max(), values.back());

  for (const double q : {0.0, 0.01, 0.1, 0.5, 0.9, 0.99, 0.999, 0.9999, 1.0}) {
    const std::uint64_t exact = exact_quantile(values, q);
    const std::uint64_t got = h->value_at_quantile(q);
    // Reported values are bucket upper bounds: never below the exact value,
    // and at most one bucket width above it.
    EXPECT_GE(got, exact) << "q=" << q;
    EXPECT_LE(static_cast<double>(got - exact),
              static_cast<double>(exact) * Hist::kMaxRelativeError + 1.0)
        << "q=" << q;
  }

  double sum = 0;
  for (const auto v : values) sum += static_cast<double>(v);
  EXPECT_NEAR(h->mean(), sum / static_cast<double>(values.size()), 1e-6 * h->mean());
}

TEST(Histogram, QuantileEdgeCases) {
  auto h = std::make_unique<Hist>();
  h->record(100);
  EXPECT_EQ(h->value_at_quantile(0.0), 100u);
  EXPECT_EQ(h->value_at_quantile(0.5), 100u);
  EXPECT_EQ(h->value_at_quantile(1.0), 100u);
  EXPECT_EQ(h->value_at_quantile(-1.0), 100u);
  EXPECT_EQ(h->value_at_quantile(2.0), 100u);
  EXPECT_EQ(h->value_at_quantile(std::nan("")), 100u);

  h->reset();
  for (int i = 1; i <= 100; ++i) h->record(static_cast<std::uint64_t>(i));
  EXPECT_EQ(h->percentile(50), 50u);
  EXPECT_EQ(h->percentile(99), 99u);
  EXPECT_EQ(h->percentile(100), 100u);
  EXPECT_EQ(h->percentile(0.5), 1u);
}

TEST(Histogram, IntegerPercentilesAreExactDespiteFloatingPoint) {
  // 0.07 * 100 == 7.000000000000001 in binary floating point; a naive ceil
  // would report p7 of 1..100 as 8. Check every integer percentile, and the
  // 10-sample case where x87 long double used to push p90 to the max.
  auto h = std::make_unique<Hist>();
  for (int i = 1; i <= 100; ++i) h->record(static_cast<std::uint64_t>(i));
  for (int p = 1; p <= 100; ++p) {
    EXPECT_EQ(h->percentile(p), static_cast<std::uint64_t>(p)) << "p" << p;
  }
  h->reset();
  for (int i = 1; i <= 10; ++i) h->record(static_cast<std::uint64_t>(i));
  for (int p = 10; p <= 100; p += 10) {
    EXPECT_EQ(h->percentile(p), static_cast<std::uint64_t>(p / 10)) << "p" << p;
  }
  // Non-integer ranks still round up: p25 of 10 samples is the 3rd value.
  EXPECT_EQ(h->percentile(25), 3u);
  EXPECT_EQ(h->percentile(99.9), 10u);
}

TEST(Histogram, OverflowIsClampedAndCounted) {
  auto h = std::make_unique<Hist>();
  const std::uint64_t huge = Hist::kMaxTrackable + 12345;
  h->record(10);
  h->record(huge);
  EXPECT_EQ(h->count(), 2u);
  EXPECT_EQ(h->overflow_count(), 1u);
  EXPECT_EQ(h->max(), huge) << "max stays exact even when the bucket is clamped";
  EXPECT_EQ(h->percentile(100), huge);
  EXPECT_EQ(h->bucket_count_at(Hist::kBucketCount - 1), 1u);
}

TEST(Histogram, MergeEqualsRecordingEverything) {
  auto a = std::make_unique<Hist>();
  auto b = std::make_unique<Hist>();
  auto all = std::make_unique<Hist>();
  std::mt19937_64 rng(7);
  for (int i = 0; i < 50'000; ++i) {
    const std::uint64_t v = rng() % 10'000'000;
    (i % 3 == 0 ? *a : *b).record(v);
    all->record(v);
  }
  a->merge(*b);
  EXPECT_EQ(a->count(), all->count());
  EXPECT_EQ(a->min(), all->min());
  EXPECT_EQ(a->max(), all->max());
  for (std::size_t i = 0; i < Hist::kBucketCount; ++i) {
    ASSERT_EQ(a->bucket_count_at(i), all->bucket_count_at(i));
  }
  for (const double p : {50.0, 99.0, 99.9}) EXPECT_EQ(a->percentile(p), all->percentile(p));

  // Merging an empty histogram must not disturb min/max.
  auto empty = std::make_unique<Hist>();
  const std::uint64_t min_before = a->min();
  a->merge(*empty);
  EXPECT_EQ(a->min(), min_before);
}

TEST(Histogram, RecordN) {
  auto h = std::make_unique<Hist>();
  h->record_n(500, 1000);
  h->record_n(9, 0);  // no-op
  EXPECT_EQ(h->count(), 1000u);
  EXPECT_EQ(h->min(), 500u);
  EXPECT_DOUBLE_EQ(h->mean(), 500.0);
}

TEST(Histogram, Full64BitRange) {
  using H64 = adsb::LogLinearHistogram<8, 64>;
  auto h = std::make_unique<H64>();
  h->record(0);
  h->record(~std::uint64_t{0});
  EXPECT_EQ(h->overflow_count(), 0u);
  EXPECT_EQ(h->max(), ~std::uint64_t{0});
  EXPECT_EQ(H64::index_of(~std::uint64_t{0}), H64::kBucketCount - 1);
  EXPECT_EQ(H64::bucket_high(H64::kBucketCount - 1), ~std::uint64_t{0});
}

}  // namespace
