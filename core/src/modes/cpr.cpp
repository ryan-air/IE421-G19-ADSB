#include "adsb/modes/cpr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace adsb::modes {

namespace {

constexpr double kCprScale = 131072.0;  // 2^17
constexpr int kNz = 15;                 // latitude zones per hemisphere quadrant

// Latitudes (degrees, absolute) at which NL drops from 59 to 58, 58 to 57, ...,
// 3 to 2. Precomputed from the closed-form NL definition; cross-checked
// against cpr_nl_reference() in tests.
constexpr std::array<double, 58> kNlTransitions = {
    10.47047130, 14.82817437, 18.18626357, 21.02939493, 23.54504487, 25.82924707,
    27.93898710, 29.91135686, 31.77209708, 33.53993436, 35.22899598, 36.85025108,
    38.41241892, 39.92256684, 41.38651832, 42.80914012, 44.19454951, 45.54626723,
    46.86733252, 48.16039128, 49.42776439, 50.67150166, 51.89342469, 53.09516153,
    54.27817472, 55.44378444, 56.59318756, 57.72747354, 58.84763776, 59.95459277,
    61.04917774, 62.13216659, 63.20427479, 64.26616523, 65.31845310, 66.36171008,
    67.39646774, 68.42322022, 69.44242631, 70.45451075, 71.45986473, 72.45884545,
    73.45177442, 74.43893416, 75.42056257, 76.39684391, 77.36789461, 78.33374083,
    79.29428225, 80.24923213, 81.19801349, 82.13956981, 83.07199445, 83.99173563,
    84.89166191, 85.75541621, 86.53536998, 87.00000000,
};

// Floating-point modulo with a result in [0, y) for y > 0. The two guards
// handle rounding in x - y * floor(x / y), which can land just outside.
double fmod_pos(double x, double y) noexcept {
  double r = x - y * std::floor(x / y);
  if (r < 0.0) r += y;
  if (r >= y) r -= y;
  return r;
}

int imod_pos(int x, int y) noexcept {
  const int r = x % y;
  return r < 0 ? r + y : r;
}

double normalize_lon(double lon) noexcept {
  lon = fmod_pos(lon + 180.0, 360.0) - 180.0;
  return lon;
}

// Shared airborne/surface global latitude computation.
// `zone_span` is 360 (airborne) or 90 (surface).
struct GlobalLat {
  double lat_even;
  double lat_odd;
};

GlobalLat global_lat(const CprFrame& even, const CprFrame& odd, double zone_span) noexcept {
  const double lat_e = even.lat / kCprScale;
  const double lat_o = odd.lat / kCprScale;
  const double dlat_e = zone_span / (4 * kNz);
  const double dlat_o = zone_span / (4 * kNz - 1);
  const int j = static_cast<int>(std::floor(59.0 * lat_e - 60.0 * lat_o + 0.5));
  return {dlat_e * (imod_pos(j, 60) + lat_e), dlat_o * (imod_pos(j, 59) + lat_o)};
}

// Global longitude from an even/odd pair, given the latitude of the newer
// frame. Result is in [0, zone_span).
double global_lon(const CprFrame& even, const CprFrame& odd, bool odd_is_newer,
                  double newer_lat, double zone_span) noexcept {
  const double lon_e = even.lon / kCprScale;
  const double lon_o = odd.lon / kCprScale;
  const int nl = cpr_nl(newer_lat);
  const int ni = std::max(nl - (odd_is_newer ? 1 : 0), 1);
  const int m = static_cast<int>(std::floor(lon_e * (nl - 1) - lon_o * nl + 0.5));
  const double frac = odd_is_newer ? lon_o : lon_e;
  return (zone_span / ni) * (imod_pos(m, ni) + frac);
}

LatLon local_decode(const CprFrame& f, const LatLon& ref, double zone_span) noexcept {
  const int i = f.odd ? 1 : 0;
  const double lat_cpr = f.lat / kCprScale;
  const double lon_cpr = f.lon / kCprScale;

  const double dlat = zone_span / (4 * kNz - i);
  const double j = std::floor(ref.lat / dlat) +
                   std::floor(fmod_pos(ref.lat, dlat) / dlat - lat_cpr + 0.5);
  const double lat = dlat * (j + lat_cpr);

  const int ni = std::max(cpr_nl(lat) - i, 1);
  const double dlon = zone_span / ni;
  const double m = std::floor(ref.lon / dlon) +
                   std::floor(fmod_pos(ref.lon, dlon) / dlon - lon_cpr + 0.5);
  const double lon = dlon * (m + lon_cpr);
  return {lat, normalize_lon(lon)};
}

}  // namespace

int cpr_nl(double lat) noexcept {
  const double a = std::fabs(lat);
  // Count how many transition latitudes are <= |lat|; NL = 59 - count.
  // The spec defines NL(87) = 2 and NL = 1 beyond 87 degrees.
  if (a >= 87.0) return a > 87.0 ? 1 : 2;
  const auto it = std::upper_bound(kNlTransitions.begin(), kNlTransitions.end(), a);
  return 59 - static_cast<int>(it - kNlTransitions.begin());
}

int cpr_nl_reference(double lat) noexcept {
  const double a = std::fabs(lat);
  if (a == 0.0) return 59;
  if (a == 87.0) return 2;
  if (a > 87.0) return 1;
  constexpr double pi = std::numbers::pi;
  const double num = 1.0 - std::cos(pi / (2.0 * kNz));
  const double cos_lat = std::cos(pi / 180.0 * a);
  const double den = cos_lat * cos_lat;
  return static_cast<int>(std::floor(2.0 * pi / std::acos(1.0 - num / den)));
}

std::optional<LatLon> cpr_global_airborne(const CprFrame& even, const CprFrame& odd,
                                          bool odd_is_newer) noexcept {
  GlobalLat g = global_lat(even, odd, 360.0);
  // Southern hemisphere latitudes come out in [270, 360).
  if (g.lat_even >= 270.0) g.lat_even -= 360.0;
  if (g.lat_odd >= 270.0) g.lat_odd -= 360.0;
  if (std::fabs(g.lat_even) > 90.0 || std::fabs(g.lat_odd) > 90.0) return std::nullopt;
  // Both frames must be in the same longitude-zone band.
  if (cpr_nl(g.lat_even) != cpr_nl(g.lat_odd)) return std::nullopt;

  const double lat = odd_is_newer ? g.lat_odd : g.lat_even;
  const double lon = global_lon(even, odd, odd_is_newer, lat, 360.0);
  return LatLon{lat, normalize_lon(lon)};
}

LatLon cpr_local_airborne(const CprFrame& f, const LatLon& ref) noexcept {
  return local_decode(f, ref, 360.0);
}

std::optional<LatLon> cpr_global_surface(const CprFrame& even, const CprFrame& odd,
                                         bool odd_is_newer, const LatLon& ref) noexcept {
  const GlobalLat g = global_lat(even, odd, 90.0);
  // Northern solution is in [0, 90); the southern one is 90 deg lower.
  // Pick whichever is closer to the reference latitude.
  auto pick = [&](double north) {
    const double south = north - 90.0;
    return std::fabs(north - ref.lat) <= std::fabs(south - ref.lat) ? north : south;
  };
  const double lat_e = pick(g.lat_even);
  const double lat_o = pick(g.lat_odd);
  if (cpr_nl(lat_e) != cpr_nl(lat_o)) return std::nullopt;

  const double lat = odd_is_newer ? lat_o : lat_e;
  const double lon0 = global_lon(even, odd, odd_is_newer, lat, 90.0);

  // Four candidates, 90 deg apart; choose the one closest to the reference.
  double best = normalize_lon(lon0);
  double best_dist = 1e9;
  for (int k = 0; k < 4; ++k) {
    const double cand = normalize_lon(lon0 + 90.0 * k);
    double d = std::fabs(cand - ref.lon);
    if (d > 180.0) d = 360.0 - d;
    if (d < best_dist) {
      best_dist = d;
      best = cand;
    }
  }
  return LatLon{lat, best};
}

LatLon cpr_local_surface(const CprFrame& f, const LatLon& ref) noexcept {
  return local_decode(f, ref, 90.0);
}

CprFrame cpr_encode(double lat, double lon, bool odd, bool surface) noexcept {
  const double span = surface ? 90.0 : 360.0;
  const int i = odd ? 1 : 0;
  const double dlat = span / (4 * kNz - i);
  const double yz = std::floor(kCprScale * fmod_pos(lat, dlat) / dlat + 0.5);
  const double rlat = dlat * (yz / kCprScale + std::floor(lat / dlat));
  const int ni = std::max(cpr_nl(rlat) - i, 1);
  const double dlon = span / ni;
  const double xz = std::floor(kCprScale * fmod_pos(lon, dlon) / dlon + 0.5);
  CprFrame f;
  f.lat = static_cast<std::uint32_t>(yz) & 0x1FFFFu;
  f.lon = static_cast<std::uint32_t>(xz) & 0x1FFFFu;
  f.odd = odd;
  return f;
}

}  // namespace adsb::modes
