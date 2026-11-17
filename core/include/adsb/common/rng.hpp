#pragma once

// Deterministic, portable random numbers.
//
// The standard <random> engines are portable but its *distributions* are not:
// libstdc++ and libc++ produce different values for the same seed. Anything
// that must be reproducible across machines (the synthetic generator, tests,
// load generation) uses this instead.

#include <cstdint>

namespace adsb {

// xoshiro256** seeded through splitmix64 (Blackman & Vigna).
class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1) noexcept {
    std::uint64_t x = seed;
    for (auto& word : s_) word = splitmix64(x);
  }

  [[nodiscard]] std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform double in [0, 1) with 53 random bits.
  [[nodiscard]] double uniform() noexcept {
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
  }

  // Uniform double in [lo, hi).
  [[nodiscard]] double uniform(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }

  // Uniform integer in [0, n). n must be > 0. (Lemire's method, unbiased.)
  [[nodiscard]] std::uint64_t below(std::uint64_t n) noexcept {
    __extension__ typedef unsigned __int128 u128;
    std::uint64_t x = next();
    u128 m = static_cast<u128>(x) * n;
    auto low = static_cast<std::uint64_t>(m);
    if (low < n) {
      const std::uint64_t threshold = (0 - n) % n;
      while (low < threshold) {
        x = next();
        m = static_cast<u128>(x) * n;
        low = static_cast<std::uint64_t>(m);
      }
    }
    return static_cast<std::uint64_t>(m >> 64);
  }

  // True with probability p.
  [[nodiscard]] bool chance(double p) noexcept { return uniform() < p; }

  // Derives an independent stream, e.g. one per aircraft.
  [[nodiscard]] Rng fork(std::uint64_t salt) noexcept { return Rng(next() ^ (salt * 0x9E3779B97F4A7C15ULL)); }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }
  static constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  std::uint64_t s_[4];
};

}  // namespace adsb
