#pragma once

// Mode S / ADS-B frame decoder.
//
// Stateless: one frame in, one fixed-size DecodedMsg out. No allocation, no
// strings, no exceptions. Stateful work (pairing CPR even/odd frames,
// validating Address/Parity addresses against known aircraft, duplicate and
// out-of-order handling) belongs to the track state engine.
//
// Coverage (README_UPDATES.md §7.1):
//   DF0/16  short/long air-air surveillance: altitude, air/ground   [A/P]
//   DF4/20  surveillance altitude reply: altitude, air/ground        [A/P]
//   DF5/21  surveillance identity reply: squawk, air/ground          [A/P]
//   DF11    all-call reply: address, capability                     [CRC, II/SI accepted]
//   DF17    ADS-B extended squitter                                  [CRC]
//   DF18    TIS-B / ADS-R / non-transponder ES (CF 0,1,2,5,6)        [CRC]
//   DF24    Comm-D (address recovered only)                          [A/P]
// ADS-B type codes: 0 (altitude only), 1-4 identification, 5-8 surface
// position, 9-18 airborne position (baro), 19 velocity (subtypes 1-4),
// 20-22 airborne position (GNSS height), 28/1 emergency + squawk.
// Not yet decoded: TC 29 (target state), TC 31 (operational status), Comm-B
// (BDS registers in DF20/21), DF19 military, single-bit error correction.
//
// [A/P] Address/Parity formats: the 24-bit address is recovered from the CRC
// syndrome and CANNOT be validated by the frame alone. Such frames are
// returned with status kAddressParity; the caller must accept them only if
// `icao` is already a live track (otherwise any corrupted frame would create a
// phantom aircraft).

#include <cstddef>
#include <cstdint>

#include "adsb/modes/cpr.hpp"

namespace adsb::modes {

enum class DecodeStatus : std::uint8_t {
  kOk,              // CRC verified (DF11/17/18); fields valid per `fields`
  kAddressParity,   // decoded, but `icao` is unverified (see header comment)
  kBadCrc,          // DF11/17/18 with a nonzero syndrome
  kBadLength,       // frame length does not match the downlink format
  kUnknownFormat,   // unassigned or unsupported downlink format
};

[[nodiscard]] const char* to_string(DecodeStatus s) noexcept;

// Bitmask of which fields of DecodedMsg are populated.
enum Field : std::uint32_t {
  kFieldAltitude       = 1u << 0,   // altitude_ft, altitude_gnss
  kFieldCpr            = 1u << 1,   // cpr, cpr_surface
  kFieldGroundSpeed    = 1u << 2,   // ground_speed_kt
  kFieldTrack          = 1u << 3,   // track_deg
  kFieldAirspeed       = 1u << 4,   // airspeed_kt, airspeed_true
  kFieldHeading        = 1u << 5,   // heading_deg
  kFieldVerticalRate   = 1u << 6,   // vertical_rate_fpm, vertical_rate_gnss
  kFieldCallsign       = 1u << 7,   // callsign
  kFieldCategory       = 1u << 8,   // category
  kFieldSquawk         = 1u << 9,   // squawk
  kFieldEmergency      = 1u << 10,  // emergency
  kFieldAirGround      = 1u << 11,  // on_ground
  kFieldGnssBaroDiff   = 1u << 12,  // gnss_baro_diff_ft
};

struct DecodedMsg {
  std::uint8_t df = 0;          // downlink format (24 for all Comm-D)
  std::uint8_t tc = 0;          // ADS-B type code (DF17/18 only)
  std::uint8_t subtype = 0;     // ADS-B subtype (velocity / TC28)
  std::uint8_t ca = 0;          // CA (DF11/17), CF (DF18), FS (DF4/5/20/21)
  std::uint32_t icao = 0;       // 24-bit address (unverified if kAddressParity)
  std::uint32_t syndrome = 0;   // CRC syndrome (0 for clean DF17/18)
  std::uint32_t fields = 0;     // Field bitmask
  bool non_icao_address = false;  // DF18 CF=1/5, or CF=2/6 with IMF=1

  // For TC 20-22 (GNSS height) the 12-bit field is decoded with the same 25 ft
  // encoding as barometric altitude, matching readsb/dump1090 (our oracle).
  // pyModeS instead reads it as metres; the readsb differential diff on real
  // traffic is the arbiter if the two ever disagree.
  std::int32_t altitude_ft = 0;
  bool altitude_gnss = false;     // true: GNSS height (TC 20-22); false: baro

  CprFrame cpr;
  bool cpr_surface = false;

  float ground_speed_kt = 0.0f;
  float track_deg = 0.0f;         // true track, [0, 360)
  float airspeed_kt = 0.0f;
  bool airspeed_true = false;     // true: TAS; false: IAS
  float heading_deg = 0.0f;       // magnetic heading, [0, 360)
  std::int32_t vertical_rate_fpm = 0;
  bool vertical_rate_gnss = false;
  std::int32_t gnss_baro_diff_ft = 0;

  char callsign[8] = {};          // space-padded, not NUL-terminated
  std::uint8_t category = 0;      // e.g. 0xA3 = category set A, value 3
  std::uint16_t squawk = 0;       // octal digits as hex nibbles: 7700 -> 0x7700
  std::uint8_t emergency = 0;     // TC28/1 emergency state (0 = none)
  bool on_ground = false;

  [[nodiscard]] bool has(Field f) const noexcept { return (fields & f) != 0; }
};

// Expected frame length in bytes (7 or 14) for a downlink format, from the
// first byte of a frame. Returns 0 for unassigned formats.
[[nodiscard]] std::size_t expected_frame_length(std::uint8_t first_byte) noexcept;

// Decodes one frame. `len` must be 7 or 14 bytes. `out` is fully overwritten.
[[nodiscard]] DecodeStatus decode(const std::uint8_t* frame, std::size_t len,
                                  DecodedMsg& out) noexcept;

// Surface movement field (7 bits) to ground speed in knots.
// Returns a negative value when no speed is available (0, or reserved codes).
[[nodiscard]] double surface_movement_to_knots(unsigned movement) noexcept;

}  // namespace adsb::modes
