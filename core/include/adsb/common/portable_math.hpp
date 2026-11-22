#pragma once

// Portable elementary functions: the same bits on every platform.
//
// libm implementations differ (glibc, Apple libm, MSVC): sin, cos, log and exp
// are not required to be correctly rounded, so the last bit of a result can
// differ between machines. The IQ generator calls them per sample (Gaussian
// noise, carrier phase) and its output is checked by hash across GCC, Clang,
// macOS and Linux, so it uses these instead. They use only +, -, *, / and
// exact operations (floor, ldexp, frexp, sqrt is correctly rounded by IEEE
// 754), and the build sets -ffp-contract=off, so results are reproducible.
//
// Accuracy: a few ulp over the ranges used here (|x| < 1e6 for sin/cos).
// Not a general-purpose libm: no special handling of inf/NaN beyond what the
// arithmetic gives.

#include <cmath>
#include <cstdint>

namespace adsb::pm {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;
inline constexpr double kLn2 = 0.69314718055994530942;
inline constexpr double kLn10 = 2.30258509299404568402;

namespace detail {

// Polynomials on |r| <= pi/4 (Taylor, terms to r^17 / r^16: error < 1e-17).
inline double sin_poly(double r) noexcept {
  const double r2 = r * r;
  double p = 1.0 / 355687428096000.0;            // 1/17!
  p = p * r2 - 1.0 / 1307674368000.0;            // 1/15!
  p = p * r2 + 1.0 / 6227020800.0;               // 1/13!
  p = p * r2 - 1.0 / 39916800.0;                 // 1/11!
  p = p * r2 + 1.0 / 362880.0;                   // 1/9!
  p = p * r2 - 1.0 / 5040.0;                     // 1/7!
  p = p * r2 + 1.0 / 120.0;                      // 1/5!
  p = p * r2 - 1.0 / 6.0;                        // 1/3!
  return r + r * r2 * p;
}

inline double cos_poly(double r) noexcept {
  const double r2 = r * r;
  double p = 1.0 / 20922789888000.0;             // 1/16!
  p = p * r2 - 1.0 / 87178291200.0;              // 1/14!
  p = p * r2 + 1.0 / 479001600.0;                // 1/12!
  p = p * r2 - 1.0 / 3628800.0;                  // 1/10!
  p = p * r2 + 1.0 / 40320.0;                    // 1/8!
  p = p * r2 - 1.0 / 720.0;                      // 1/6!
  p = p * r2 + 1.0 / 24.0;                       // 1/4!
  p = p * r2 - 0.5;                              // 1/2!
  return 1.0 + r2 * p;
}

}  // namespace detail

// sin and cos of x together. Cody-Waite reduction by pi/2 with a three-part
// constant (exact products for |k| < 2^20).
inline void sincos(double x, double& s, double& c) noexcept {
  constexpr double kTwoOverPi = 0.63661977236758134308;
  constexpr double kPio2a = 1.57079632673412561417e+00;   // first 33 bits of pi/2
  constexpr double kPio2b = 6.07710050650619224932e-11;   // next 33 bits
  constexpr double kPio2c = 2.02226624879595063154e-21;   // the rest
  const double k = std::floor(x * kTwoOverPi + 0.5);
  const double r = ((x - k * kPio2a) - k * kPio2b) - k * kPio2c;
  const double sr = detail::sin_poly(r);
  const double cr = detail::cos_poly(r);
  // Quadrant from k mod 4 (k is an exact integer in double).
  const auto q = static_cast<std::int64_t>(k) & 3;
  switch (q) {
    case 0: s = sr; c = cr; break;
    case 1: s = cr; c = -sr; break;
    case 2: s = -sr; c = -cr; break;
    default: s = -cr; c = sr; break;
  }
}

inline double sin(double x) noexcept { double s, c; sincos(x, s, c); return s; }
inline double cos(double x) noexcept { double s, c; sincos(x, s, c); return c; }

// Natural log for x > 0 (x <= 0 returns -inf / NaN like libm).
inline double log(double x) noexcept {
  if (!(x > 0.0)) return x == 0.0 ? -HUGE_VAL : (x - x) / 0.0;
  if (x == HUGE_VAL) return x;
  int e = 0;
  double m = std::frexp(x, &e);                  // x = m * 2^e, m in [0.5, 1)
  if (m < 0.70710678118654752440) { m *= 2.0; --e; }  // m in [sqrt(1/2), sqrt(2))
  // log(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| < 0.1716.
  const double s = (m - 1.0) / (m + 1.0);
  const double s2 = s * s;
  double p = 1.0 / 23.0;
  p = p * s2 + 1.0 / 21.0;
  p = p * s2 + 1.0 / 19.0;
  p = p * s2 + 1.0 / 17.0;
  p = p * s2 + 1.0 / 15.0;
  p = p * s2 + 1.0 / 13.0;
  p = p * s2 + 1.0 / 11.0;
  p = p * s2 + 1.0 / 9.0;
  p = p * s2 + 1.0 / 7.0;
  p = p * s2 + 1.0 / 5.0;
  p = p * s2 + 1.0 / 3.0;
  const double log_m = 2.0 * s + 2.0 * s * s2 * p;
  return static_cast<double>(e) * kLn2 + log_m;
}

// e^x (overflow/underflow saturate through ldexp).
inline double exp(double x) noexcept {
  if (x > 709.782712893384) return HUGE_VAL;
  if (x < -745.0) return 0.0;
  const double k = std::floor(x / kLn2 + 0.5);
  // r = x - k ln2 in two parts for accuracy; |r| <= ln2/2.
  constexpr double kLn2Hi = 6.93147180369123816490e-01;
  constexpr double kLn2Lo = 1.90821492927058770002e-10;
  const double r = (x - k * kLn2Hi) - k * kLn2Lo;
  // Taylor to r^14 on |r| <= 0.347: error < 1e-17.
  double p = 1.0 / 87178291200.0;
  p = p * r + 1.0 / 6227020800.0;
  p = p * r + 1.0 / 479001600.0;
  p = p * r + 1.0 / 39916800.0;
  p = p * r + 1.0 / 3628800.0;
  p = p * r + 1.0 / 362880.0;
  p = p * r + 1.0 / 40320.0;
  p = p * r + 1.0 / 5040.0;
  p = p * r + 1.0 / 720.0;
  p = p * r + 1.0 / 120.0;
  p = p * r + 1.0 / 24.0;
  p = p * r + 1.0 / 6.0;
  p = p * r + 0.5;
  p = p * r + 1.0;
  p = p * r + 1.0;
  return std::ldexp(p, static_cast<int>(k));
}

inline double log10(double x) noexcept { return log(x) / kLn10; }

// 10^(db/10) and back.
inline double db_to_power(double db) noexcept { return exp(db * (kLn10 / 10.0)); }
inline double power_to_db(double p) noexcept { return 10.0 * log10(p); }

// atan2 (y, x) via a reduced argument and an odd series; used for geometry
// (elevation angles), not per sample.
inline double atan(double x) noexcept {
  // Reduce: atan(x) = pi/2 - atan(1/x) for |x| > 1; then halve the argument
  // twice with atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) so |x| <= 0.2.
  bool invert = false;
  double sign = 1.0;
  if (x < 0) { x = -x; sign = -1.0; }
  if (x > 1.0) { x = 1.0 / x; invert = true; }
  x = x / (1.0 + std::sqrt(1.0 + x * x));
  x = x / (1.0 + std::sqrt(1.0 + x * x));
  const double x2 = x * x;
  double p = -1.0 / 19.0;
  p = p * x2 + 1.0 / 17.0;
  p = p * x2 - 1.0 / 15.0;
  p = p * x2 + 1.0 / 13.0;
  p = p * x2 - 1.0 / 11.0;
  p = p * x2 + 1.0 / 9.0;
  p = p * x2 - 1.0 / 7.0;
  p = p * x2 + 1.0 / 5.0;
  p = p * x2 - 1.0 / 3.0;
  double a = 4.0 * (x + x * x2 * p);
  if (invert) a = kPi / 2 - a;
  return sign * a;
}

inline double atan2(double y, double x) noexcept {
  if (x > 0) return atan(y / x);
  if (x < 0) return y >= 0 ? atan(y / x) + kPi : atan(y / x) - kPi;
  if (y > 0) return kPi / 2;
  if (y < 0) return -kPi / 2;
  return 0.0;
}

}  // namespace adsb::pm
