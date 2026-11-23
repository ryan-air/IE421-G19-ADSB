#pragma once

// Mode S / ADS-B frame encoders: the inverse of decoder.hpp.
//
// Used by the synthetic traffic generator (tools/adsb-synth) and by tests.
// Every encoder produces a frame with correct parity, which the decoder must
// read back exactly (tests/test_encoder.cpp checks this round trip, and the
// decoder itself is checked against published vectors).

#include <cstddef>
#include <cstdint>
#include <optional>

#include "adsb/modes/cpr.hpp"
#include "adsb/modes/decoder.hpp"

namespace adsb::modes {

struct Frame {
  std::uint8_t bytes[14] = {};
  std::uint8_t len = 0;  // 7 or 14
};

// Writes parity = CRC24(data) XOR overlay into the last three bytes.
// overlay = 0 for DF17/18, the II code for DF11, the address for DF4/5/20/21.
void write_parity(std::uint8_t* frame, std::size_t len, std::uint32_t overlay) noexcept;

// DF17 identification (TC 1-4). `category` uses the decoder's encoding, e.g.
// 0xA3 = set A, category 3 (TC 4, CA 3). Characters outside the 6-bit
// callsign alphabet are sent as spaces.
[[nodiscard]] Frame encode_identification(std::uint32_t icao, const char callsign[8],
                                          std::uint8_t category, unsigned ca = 5) noexcept;

// DF17 airborne position. TC 9-18 (baro) or 20-22 (GNSS). Altitude outside
// the 25 ft code's range is sent as "not available". `nic_b` is ME bit 8
// (NIC supplement-B in version 2, the single antenna flag before).
[[nodiscard]] Frame encode_airborne_position(std::uint32_t icao, std::int32_t alt_ft,
                                             const CprFrame& cpr, unsigned tc = 11,
                                             unsigned ca = 5, bool nic_b = false) noexcept;

// DF17 surface position (TC 5-8). `track_deg` < 0 means "track not valid".
[[nodiscard]] Frame encode_surface_position(std::uint32_t icao, double ground_speed_kt,
                                            double track_deg, const CprFrame& cpr,
                                            unsigned tc = 7, unsigned ca = 4) noexcept;

// DF17 airborne velocity, subtype 1 (ground speed). East/north components in
// knots (|v| <= 1021), vertical rate in ft/min (rounded to 64 ft/min).
[[nodiscard]] Frame encode_velocity(std::uint32_t icao, double east_kt, double north_kt,
                                    std::int32_t vertical_rate_fpm, bool vr_baro = true,
                                    unsigned ca = 5) noexcept;

// DF17 airborne velocity, subtype 3 (airspeed and heading, subsonic).
// `heading_deg` < 0 means "heading not available"; airspeed in knots
// (<= 1022), true or indicated.
[[nodiscard]] Frame encode_airspeed_heading(std::uint32_t icao, double heading_deg, double airspeed_kt,
                                            bool tas, std::int32_t vertical_rate_fpm,
                                            unsigned ca = 5) noexcept;

// DF17 aircraft operational status (TC 31), the fields decode() reads back:
// subtype from `os.surface`, capability class, operational mode, version,
// and from version 1 NIC-A, NACp, SIL, NIC-baro (airborne) and HRD; from
// version 2 GVA (airborne), SIL supplement and NIC-C (surface, capability
// class bit 20, which overrides that bit of `os.capability`).
[[nodiscard]] Frame encode_operational_status(std::uint32_t icao, const OperationalStatus& os,
                                              unsigned ca = 5) noexcept;

// DF11 all-call reply (acquisition squitter when iid = 0).
[[nodiscard]] Frame encode_df11(std::uint32_t icao, unsigned ca = 5, unsigned iid = 0) noexcept;

// DF4 surveillance altitude reply (Address/Parity). fs: flight status.
[[nodiscard]] Frame encode_df4(std::uint32_t icao, std::int32_t alt_ft, unsigned fs = 0) noexcept;

// DF5 surveillance identity reply (Address/Parity). squawk: 0xABCD octal digits.
[[nodiscard]] Frame encode_df5(std::uint32_t icao, std::uint16_t squawk, unsigned fs = 0) noexcept;

// Ground speed in knots -> 7-bit surface movement code (nearest step).
[[nodiscard]] unsigned knots_to_surface_movement(double kt) noexcept;

}  // namespace adsb::modes
