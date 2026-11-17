#pragma once

// Fixed-memory log-linear latency histogram (HdrHistogram-style).
//
// Values are unsigned integers (typically nanoseconds or ticks). Every
// power-of-two range ("octave") is split into 2^(SubBits-1) equal-width
// buckets, so the relative error of any reported value is bounded by
//
//     relative_error <= 2^-(SubBits-1)        (SubBits = 10 -> 0.196%)
//
// and values below 2^SubBits are recorded exactly.
//
// Bucket layout. Let H = 2^(SubBits-1). For a value v with highest set bit
// `msb`, shift = max(0, msb - SubBits + 1) and top = v >> shift. Then
// top is in [0, 2H) when shift == 0 and in [H, 2H) otherwise, and
//     index = shift * H + top
// gives a contiguous index space. The bucket covers
//     [top << shift, ((top + 1) << shift) - 1].
//
// Values above 2^MaxBits - 1 are clamped into the last bucket and counted in
// overflow_count(); min()/max() remain exact.
//
// record() is branch-light, allocation-free, and touches one counter, so it is
// safe on hot paths. A histogram is NOT thread-safe: keep one per thread and
// merge() them after the run.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace adsb {

template <unsigned SubBits = 10, unsigned MaxBits = 40>
class LogLinearHistogram {
  static_assert(SubBits >= 2 && SubBits <= 20, "SubBits out of range");
  static_assert(MaxBits > SubBits && MaxBits <= 64, "MaxBits out of range");

 public:
  static constexpr std::uint64_t kHalf = std::uint64_t{1} << (SubBits - 1);
  static constexpr std::uint64_t kMaxTrackable =
      MaxBits == 64 ? std::numeric_limits<std::uint64_t>::max()
                    : (std::uint64_t{1} << MaxBits) - 1;
  // Number of buckets needed to cover [0, kMaxTrackable].
  static constexpr std::size_t kBucketCount =
      static_cast<std::size_t>((MaxBits - SubBits + 2) * kHalf);
  static constexpr double kMaxRelativeError = 1.0 / static_cast<double>(kHalf);

  LogLinearHistogram() { reset(); }

  void reset() noexcept {
    counts_.fill(0);
    total_ = 0;
    overflow_ = 0;
    min_ = std::numeric_limits<std::uint64_t>::max();
    max_ = 0;
    sum_ = 0;
  }

  void record(std::uint64_t value) noexcept { record_n(value, 1); }

  void record_n(std::uint64_t value, std::uint64_t n) noexcept {
    if (n == 0) return;
    if (value < min_) min_ = value;
    if (value > max_) max_ = value;
    std::uint64_t clamped = value;
    if (value > kMaxTrackable) {
      clamped = kMaxTrackable;
      overflow_ += n;
    }
    counts_[index_of(clamped)] += n;
    total_ += n;
    sum_ += static_cast<long double>(value) * static_cast<long double>(n);
  }

  // Adds all of `other`'s samples into this histogram.
  void merge(const LogLinearHistogram& other) noexcept {
    for (std::size_t i = 0; i < kBucketCount; ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
    overflow_ += other.overflow_;
    sum_ += other.sum_;
    if (other.total_ != 0) {
      if (other.min_ < min_) min_ = other.min_;
      if (other.max_ > max_) max_ = other.max_;
    }
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] double mean() const noexcept {
    return total_ ? static_cast<double>(sum_ / static_cast<long double>(total_)) : 0.0;
  }

  // Value at quantile q in [0, 1], using the nearest-rank definition:
  // the smallest recorded value v such that at least ceil(q * count) samples
  // are <= v. Reported as the bucket's highest value (clamped to the exact
  // recorded max), matching HdrHistogram's "highest equivalent value"
  // convention, so percentiles never under-report latency.
  [[nodiscard]] std::uint64_t value_at_quantile(double q) const noexcept {
    if (total_ == 0) return 0;
    if (!(q > 0.0)) return min_;  // also handles NaN
    if (q >= 1.0) return max_;
    // rank = ceil(q * count). q is usually a decimal (0.07, 0.999) that binary
    // floating point cannot represent exactly, so q * count can land a hair
    // above an integer (0.07 * 100 = 7.000000000000001) and ceil would step
    // one rank too far. Treat products within a relative 1e-9 of an integer as
    // that integer before taking the ceiling.
    const long double exact_rank = static_cast<long double>(q) * static_cast<long double>(total_);
    const long double nearest = std::nearbyint(exact_rank);
    std::uint64_t rank;
    if (std::fabs(exact_rank - nearest) <= 1e-9L * (nearest > 1.0L ? nearest : 1.0L)) {
      rank = static_cast<std::uint64_t>(nearest);
    } else {
      rank = static_cast<std::uint64_t>(std::ceil(exact_rank));
    }
    if (rank == 0) rank = 1;
    if (rank > total_) rank = total_;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
      seen += counts_[i];
      if (seen >= rank) {
        // The exact max is known, so never report above it.
        const std::uint64_t hi = bucket_high(i);
        return hi > max_ ? max_ : hi;
      }
    }
    return max_;  // unreachable when counts are consistent
  }

  // Convenience: percentile in [0, 100].
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
    return value_at_quantile(p / 100.0);
  }

  [[nodiscard]] std::uint64_t bucket_count_at(std::size_t index) const noexcept {
    return counts_[index];
  }

  // ---- Bucket math (public for testing) -----------------------------------

  static constexpr std::size_t index_of(std::uint64_t v) noexcept {
    const unsigned msb = v == 0 ? 0u : 63u - static_cast<unsigned>(std::countl_zero(v));
    const unsigned shift = msb >= SubBits ? msb - SubBits + 1 : 0u;
    const std::uint64_t top = v >> shift;
    return static_cast<std::size_t>(shift * kHalf + top);
  }

  static constexpr std::uint64_t bucket_low(std::size_t index) noexcept {
    const auto [shift, top] = decompose(index);
    return top << shift;
  }

  static constexpr std::uint64_t bucket_high(std::size_t index) noexcept {
    const auto [shift, top] = decompose(index);
    // ((top + 1) << shift) - 1, written to avoid overflow at the very top.
    return (top << shift) + ((std::uint64_t{1} << shift) - 1);
  }

 private:
  struct ShiftTop {
    unsigned shift;
    std::uint64_t top;
  };

  static constexpr ShiftTop decompose(std::size_t index) noexcept {
    const auto i = static_cast<std::uint64_t>(index);
    if (i < 2 * kHalf) return {0u, i};
    // For shift >= 1, index = shift * H + top with top in [H, 2H):
    // shift = index / H - 1, top = index % H + H.
    const auto shift = static_cast<unsigned>(i / kHalf - 1);
    return {shift, i % kHalf + kHalf};
  }

  std::array<std::uint64_t, kBucketCount> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t overflow_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
  long double sum_ = 0;
};

// Default used throughout the pipeline: ~0.2% precision, 1 ns .. ~18 minutes.
using LatencyHistogram = LogLinearHistogram<10, 40>;

}  // namespace adsb
