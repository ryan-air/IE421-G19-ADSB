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
// 20-22 airborne position (GNSS height), 28/1 emergency + squawk, 29/1
// target state and status (DO-260B), 31 operational status (version, and
// for versions 1-2 the NIC supplement, NACp, SIL, GVA, NIC-baro, HRD).
// Comm-B (DF20/21): the MB field's register (BDS) is not sent with the
// reply, so it is inferred: each of BDS 1,0 1,7 2,0 3,0 4,0 5,0 6,0 whose
// format rules the MB field satisfies is a candidate (CommB::candidates);
// values are decoded only when exactly one candidate remains (5,0 and 6,0
// are often both plausible: then nothing is decoded). Comm-B values are
// kept apart from the ADS-B fields (DecodedMsg::comm_b) and the track engine
// does not use them yet.
// Optional (DecodeOptions::fix_single_bit, off by default): a DF17/18 frame
// whose syndrome is that of one wrong bit outside the DF field (bits 6-112)
// is repaired, as readsb does by default; DecodedMsg::corrected_bit says
// which. Any frame one bit from a valid one passes, so repaired frames are
// as weak as Address/Parity ones: the track engine applies them only to
// tracks a clean frame confirmed. Address/Parity formats cannot be repaired (their syndrome is the
// address). Not decoded: DF19 military, TC 29 subtype 0 (DO-260A), BDS 4,4
// and other registers.
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
  kFieldTargetState    = 1u << 13,  // target (TC 29 subtype 1)
  kFieldOpStatus       = 1u << 14,  // op_status (TC 31)
  kFieldCommB          = 1u << 15,  // comm_b (DF20/21: candidates always; values if unique)
};

struct DecodeOptions {
  bool fix_single_bit = false;      // repair one wrong bit in DF17/18 (see header comment)
};

// TC 29 subtype 1 (DO-260B §2.2.3.2.7.1): what the crew or FMS selected.
struct TargetState {
  enum : std::uint16_t {
    kAltitude = 1u << 0,    // selected_altitude_ft, altitude_from_fms
    kBaro = 1u << 1,        // baro_setting_hpa
    kHeading = 1u << 2,     // selected_heading_deg
    kModes = 1u << 3,       // autopilot, vnav, altitude_hold, approach, lnav
  };
  std::uint16_t valid = 0;
  std::int32_t selected_altitude_ft = 0;
  bool altitude_from_fms = false;    // false: MCP/FCU
  float baro_setting_hpa = 0.0f;
  float selected_heading_deg = 0.0f;
  bool autopilot = false, vnav = false, altitude_hold = false, approach = false, lnav = false;
  bool tcas_operational = false;     // always present
  std::uint8_t nac_p = 0;
  bool nic_baro = false;
  std::uint8_t sil = 0;
  bool sil_per_sample = false;       // SIL supplement: false = per hour
};

// TC 31 (DO-260B §2.2.3.2.7.2). For version 0 only `version` and the raw
// capability fields mean anything.
struct OperationalStatus {
  bool surface = false;              // subtype 1
  std::uint8_t version = 0;          // 0 DO-260, 1 DO-260A, 2 DO-260B
  std::uint16_t capability = 0;      // raw capability class (16 bits airborne, 12 surface)
  std::uint16_t operational_mode = 0;  // raw operational mode (16 bits)
  bool nic_supplement_a = false;     // version >= 1
  bool nic_supplement_c = false;     // version 2, surface (capability class bit 20)
  bool nic_supplement_c_valid = false;  // NIC-C was read (version 2, surface, class code 0)
  std::uint8_t nac_p = 0;
  std::uint8_t sil = 0;
  bool sil_per_sample = false;       // version 2
  std::uint8_t gva = 0;              // version 2, airborne
  bool nic_baro = false;             // airborne
  bool heading_true_north = true;    // HRD bit 0 = true north, 1 = magnetic
};

// Comm-B MB field (DF20/21), see the header comment.
struct CommB {
  enum Register : std::uint16_t {
    kBds10 = 1u << 0,  // data link capability
    kBds17 = 1u << 1,  // common usage GICB capability
    kBds20 = 1u << 2,  // aircraft identification
    kBds30 = 1u << 3,  // ACAS active resolution advisory
    kBds40 = 1u << 4,  // selected vertical intention
    kBds50 = 1u << 5,  // track and turn
    kBds60 = 1u << 6,  // heading and speed
  };
  enum Value : std::uint32_t {
    kCallsign = 1u << 0,         // 2,0
    kMcpAltitude = 1u << 1,      // 4,0
    kFmsAltitude = 1u << 2,      // 4,0
    kBaroSetting = 1u << 3,      // 4,0
    kRoll = 1u << 4,             // 5,0
    kTrueTrack = 1u << 5,        // 5,0
    kGroundSpeed = 1u << 6,      // 5,0
    kTrackRate = 1u << 7,        // 5,0
    kTrueAirspeed = 1u << 8,     // 5,0
    kMagneticHeading = 1u << 9,  // 6,0
    kIndicatedAirspeed = 1u << 10,  // 6,0
    kMach = 1u << 11,            // 6,0
    kBaroVerticalRate = 1u << 12,      // 6,0
    kInertialVerticalRate = 1u << 13,  // 6,0
  };
  std::uint16_t candidates = 0;  // Register bits whose format rules the MB satisfies
  std::uint8_t bds = 0;          // the register if exactly one candidate (e.g. 0x50), else 0
  std::uint32_t valid = 0;       // Value bits
  char callsign[8] = {};
  std::int32_t mcp_altitude_ft = 0, fms_altitude_ft = 0;
  float baro_setting_hpa = 0.0f;
  float roll_deg = 0.0f;         // + = right wing down
  float true_track_deg = 0.0f;   // [0, 360)
  float ground_speed_kt = 0.0f;
  float track_rate_deg_s = 0.0f;
  float true_airspeed_kt = 0.0f;
  float magnetic_heading_deg = 0.0f;  // [0, 360)
  float indicated_airspeed_kt = 0.0f;
  float mach = 0.0f;
  std::int32_t baro_vertical_rate_fpm = 0, inertial_vertical_rate_fpm = 0;
};

struct DecodedMsg {
  std::uint8_t df = 0;          // downlink format (24 for all Comm-D)
  std::uint8_t tc = 0;          // ADS-B type code (DF17/18 only)
  std::uint8_t subtype = 0;     // ADS-B subtype (velocity / TC28)
  std::uint8_t ca = 0;          // CA (DF11/17), CF (DF18), FS (DF4/5/20/21)
  std::uint32_t icao = 0;       // 24-bit address (unverified if kAddressParity)
  std::uint32_t syndrome = 0;   // CRC syndrome (0 for clean DF17/18; a repaired one keeps the received syndrome)
  std::uint32_t fields = 0;     // Field bitmask
  bool non_icao_address = false;  // DF18 CF=1/5, or CF=2/6 with IMF=1
  std::int8_t corrected_bit = -1; // bit repaired by fix_single_bit (0 = first bit), else -1

  // For TC 20-22 (GNSS height) the 12-bit field is decoded with the same 25 ft
  // encoding as barometric altitude, matching readsb/dump1090 (our oracle).
  // pyModeS instead reads it as metres; the readsb differential diff on real
  // traffic is the arbiter if the two ever disagree.
  std::int32_t altitude_ft = 0;
  bool altitude_gnss = false;     // true: GNSS height (TC 20-22); false: baro

  CprFrame cpr;
  bool cpr_surface = false;
  // Airborne position ME bit 8: NIC supplement-B in version 2, the single
  // antenna flag in versions 0 and 1 (readsb reads it as NIC-B in every
  // version; position_nic() decides where it counts).
  bool nic_supplement_b = false;

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

  TargetState target;
  OperationalStatus op_status;
  CommB comm_b;

  [[nodiscard]] bool has(Field f) const noexcept { return (fields & f) != 0; }
};

// Expected frame length in bytes (7 or 14) for a downlink format, from the
// first byte of a frame. Returns 0 for unassigned formats.
[[nodiscard]] std::size_t expected_frame_length(std::uint8_t first_byte) noexcept;

// Navigation integrity category (0-11, 0 = unknown) of a position message with
// type code `tc` (5-8 surface, 9-18 / 20-22 airborne), for an aircraft that
// reported ADS-B `version` (0 until a TC 31 says otherwise) and the NIC
// supplement bits A and C (TC 31) and B (the position message itself). This is
// readsb's compute_nic() (track.c, v3.16.21), itself DO-260B Tables 2-14 and
// 2-15 with the version 0 / 1 readings; any other type code gives 0.
[[nodiscard]] std::uint8_t position_nic(unsigned tc, unsigned version, bool nic_a, bool nic_b,
                                        bool nic_c) noexcept;

// Decodes one frame. `len` must be 7 or 14 bytes. `out` is fully overwritten.
[[nodiscard]] DecodeStatus decode(const std::uint8_t* frame, std::size_t len,
                                  DecodedMsg& out, const DecodeOptions& options = {}) noexcept;

// The Comm-B part alone: candidates and values for a 56-bit MB field (the
// 7 bytes at frame[4..10] of a DF20/21 reply).
void decode_comm_b(const std::uint8_t* mb, CommB& out) noexcept;

// Surface movement field (7 bits) to ground speed in knots.
// Returns a negative value when no speed is available (0, or reserved codes).
[[nodiscard]] double surface_movement_to_knots(unsigned movement) noexcept;

}  // namespace adsb::modes
