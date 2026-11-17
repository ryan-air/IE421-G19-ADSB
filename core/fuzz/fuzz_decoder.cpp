// Fuzzes the Mode S decoder and CPR position decoding.
//
// Input layout: byte 0 selects options; the rest is the frame. When the
// "fix parity" option is set the harness rewrites the CRC so that the fuzzer
// reaches the ME-field decoding paths instead of stopping at kBadCrc.
//
// Invariants:
//  * no crash / UB;
//  * kOk for DF17/18 implies a zero syndrome; kBadCrc/kBadLength/
//    kUnknownFormat report no fields;
//  * decoded values are finite and inside physical ranges;
//  * CPR decoding of the decoded fields yields finite coordinates in range.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "adsb/modes/cpr.hpp"
#include "adsb/modes/crc24.hpp"
#include "adsb/modes/decoder.hpp"
#include "fuzz_check.hpp"

using namespace adsb::modes;

namespace {

bool in_range(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool in_range(float v, double lo, double hi) { return in_range(static_cast<double>(v), lo, hi); }

void check_latlon(const LatLon& p) {
  FUZZ_CHECK(in_range(p.lat, -90.0, 90.0));
  FUZZ_CHECK(std::isfinite(p.lon) && p.lon >= -180.0 && p.lon < 180.0);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) return 0;
  const std::uint8_t opts = data[0];
  std::uint8_t frame[14] = {};
  const std::size_t len = std::min<std::size_t>(size - 1, 14);
  std::memcpy(frame, data + 1, len);

  // Option bit 0: use the length implied by the DF instead of the input length.
  std::size_t use_len = len;
  if ((opts & 1u) && len >= 1) {
    const std::size_t want = expected_frame_length(frame[0]);
    if (want != 0) use_len = want;
  }
  // Option bit 1: rewrite parity so the CRC check passes.
  if ((opts & 2u) && (use_len == 7 || use_len == 14)) {
    const std::uint32_t p = crc24(frame, use_len - 3);
    frame[use_len - 3] = static_cast<std::uint8_t>(p >> 16);
    frame[use_len - 2] = static_cast<std::uint8_t>(p >> 8);
    frame[use_len - 1] = static_cast<std::uint8_t>(p);
  }

  DecodedMsg m;
  const DecodeStatus st = decode(frame, use_len, m);

  if (st == DecodeStatus::kBadCrc || st == DecodeStatus::kBadLength ||
      st == DecodeStatus::kUnknownFormat) {
    FUZZ_CHECK(m.fields == 0);
    return 0;
  }
  if (st == DecodeStatus::kOk && (m.df == 17 || m.df == 18)) FUZZ_CHECK(m.syndrome == 0);
  FUZZ_CHECK(m.icao <= 0xFFFFFFu);

  if (m.has(kFieldAltitude)) FUZZ_CHECK(m.altitude_ft >= -1200 && m.altitude_ft <= 126'700);
  if (m.has(kFieldGroundSpeed)) FUZZ_CHECK(in_range(m.ground_speed_kt, 0.0, 6000.0));
  if (m.has(kFieldTrack)) FUZZ_CHECK(in_range(m.track_deg, 0.0, 359.9999));
  if (m.has(kFieldHeading)) FUZZ_CHECK(in_range(m.heading_deg, 0.0, 359.9999));
  if (m.has(kFieldAirspeed)) FUZZ_CHECK(in_range(m.airspeed_kt, 0.0, 4100.0));
  if (m.has(kFieldVerticalRate)) FUZZ_CHECK(std::abs(m.vertical_rate_fpm) <= 32'640);
  if (m.has(kFieldCallsign)) {
    for (const char c : m.callsign) {
      FUZZ_CHECK(c == ' ' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    }
  }

  if (m.has(kFieldCpr)) {
    FUZZ_CHECK(m.cpr.lat < (1u << 17) && m.cpr.lon < (1u << 17));
    // Reference position derived from the options byte, anywhere on Earth.
    const LatLon ref{(opts % 170) - 85.0, (opts * 7 % 360) - 180.0};
    if (m.cpr_surface) {
      check_latlon(cpr_local_surface(m.cpr, ref));
    } else {
      check_latlon(cpr_local_airborne(m.cpr, ref));
    }
    // Pair the frame with a copy of itself flipped to the other format.
    CprFrame other = m.cpr;
    other.odd = !other.odd;
    other.lat = (other.lat * 2654435761u) & 0x1FFFFu;
    const CprFrame& e = m.cpr.odd ? other : m.cpr;
    const CprFrame& o = m.cpr.odd ? m.cpr : other;
    if (m.cpr_surface) {
      if (const auto g = cpr_global_surface(e, o, m.cpr.odd, ref)) check_latlon(*g);
    } else {
      if (const auto g = cpr_global_airborne(e, o, m.cpr.odd)) check_latlon(*g);
    }
  }
  return 0;
}
