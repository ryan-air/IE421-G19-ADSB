#pragma once

// Compact Position Reporting (CPR) decoding for ADS-B airborne and surface
// positions (ICAO Doc 9871 / DO-260B; worked examples in J. Sun, "The 1090 MHz
// Riddle", ch. 5-6).
//
// A CPR position is two 17-bit fractions (lat, lon) plus a format bit:
// even (F = 0) or odd (F = 1). Positions are resolved either
//  * globally, from one even and one odd frame received close together
//    (airborne: within 10 s; caller enforces the window), or
//  * locally, from one frame plus a reference position known to be within
//    half a zone (airborne: 180 NM, surface: 45 NM).
//
// Surface positions use zones a quarter the size (90 deg instead of 360 deg)
// and so are ambiguous by 90 deg; a reference position (the receiver)
// resolves the ambiguity.
//
// No trigonometry is evaluated: NL(lat) comes from a precomputed table of
// zone-transition latitudes.

#include <cstdint>
#include <optional>

namespace adsb::modes {

struct CprFrame {
  std::uint32_t lat = 0;  // 17-bit encoded latitude fraction
  std::uint32_t lon = 0;  // 17-bit encoded longitude fraction
  bool odd = false;       // F bit: false = even, true = odd
};

struct LatLon {
  double lat = 0.0;  // degrees, [-90, 90]
  double lon = 0.0;  // degrees, [-180, 180)
};

// Number of longitude zones at a given latitude (1..59). Table-based.
[[nodiscard]] int cpr_nl(double lat) noexcept;

// Reference implementation of NL from the closed-form definition (uses acos /
// cos). For tests only; not used on the hot path.
[[nodiscard]] int cpr_nl_reference(double lat) noexcept;

// Airborne global decode. `odd_is_newer` selects which frame's position is
// returned (the most recently received one). Returns nullopt if the two
// frames straddle a latitude-zone boundary (NL differs) and cannot be paired.
[[nodiscard]] std::optional<LatLon> cpr_global_airborne(const CprFrame& even, const CprFrame& odd,
                                                        bool odd_is_newer) noexcept;

// Airborne local decode relative to `ref` (must be within 180 NM).
[[nodiscard]] LatLon cpr_local_airborne(const CprFrame& f, const LatLon& ref) noexcept;

// Surface global decode. `ref` (normally the receiver position) selects among
// the four 90-degree-ambiguous solutions and must be within 45 NM.
[[nodiscard]] std::optional<LatLon> cpr_global_surface(const CprFrame& even, const CprFrame& odd,
                                                       bool odd_is_newer,
                                                       const LatLon& ref) noexcept;

// Surface local decode relative to `ref` (must be within 45 NM).
[[nodiscard]] LatLon cpr_local_surface(const CprFrame& f, const LatLon& ref) noexcept;

// Encoder, used by tests for round-trip checks and (later) by the synthetic
// load generator. Produces the 17-bit values as transmitted. For surface
// positions this is equivalent to DO-260B's 19-bit encoding over 360-degree
// zones truncated to its 17 low bits (= 17 bits over 90-degree zones).
[[nodiscard]] CprFrame cpr_encode(double lat, double lon, bool odd, bool surface) noexcept;

}  // namespace adsb::modes
