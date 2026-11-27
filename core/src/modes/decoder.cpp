#include "adsb/modes/decoder.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

#include "adsb/common/int128.hpp"
#include "adsb/modes/altitude.hpp"
#include "adsb/modes/crc24.hpp"

namespace adsb::modes {

namespace {

// The whole frame left-aligned in a 128-bit register, so any field is one
// shift and one mask. Bit numbering follows the ICAO documents: bit 1 is the
// first transmitted bit.
class FrameBits {
 public:
  FrameBits(const std::uint8_t* frame, std::size_t len) noexcept {
    uint128 v = 0;
    for (std::size_t i = 0; i < len; ++i) v = (v << 8) | frame[i];
    bits_ = v << (128 - 8 * len);
  }

  // Bits [first, last], 1-indexed, inclusive. Precondition: field <= 64 bits.
  [[nodiscard]] std::uint64_t get(unsigned first, unsigned last) const noexcept {
    const unsigned width = last - first + 1;
    const uint128 shifted = bits_ >> (128 - last);
    const std::uint64_t mask = width == 64 ? ~std::uint64_t{0}
                                           : (std::uint64_t{1} << width) - 1;
    return static_cast<std::uint64_t>(shifted) & mask;
  }

  [[nodiscard]] unsigned bit(unsigned n) const noexcept {
    return static_cast<unsigned>(get(n, n));
  }

  // ME field (DF17/18 bits 33-88) accessors, 1-indexed within ME.
  [[nodiscard]] std::uint64_t me(unsigned first, unsigned last) const noexcept {
    return get(32 + first, 32 + last);
  }
  [[nodiscard]] unsigned me_bit(unsigned n) const noexcept { return bit(32 + n); }

 private:
  uint128 bits_ = 0;
};

constexpr char kCallsignChars[] =
    "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
static_assert(sizeof(kCallsignChars) == 65, "callsign charset must have 64 entries");

template <typename T>
constexpr std::uint8_t u8(T v) noexcept {
  return static_cast<std::uint8_t>(v);
}
template <typename T>
constexpr std::uint16_t u16(T v) noexcept {
  return static_cast<std::uint16_t>(v);
}

void set_altitude(DecodedMsg& out, std::optional<std::int32_t> alt, bool gnss) noexcept {
  if (alt) {
    out.altitude_ft = *alt;
    out.altitude_gnss = gnss;
    out.fields |= kFieldAltitude;
  }
}

void set_ground_from_ca(DecodedMsg& out, unsigned ca) noexcept {
  // Capability: 4 = Level 2+ transponder on ground, 5 = airborne.
  if (ca == 4 || ca == 5) {
    out.on_ground = (ca == 4);
    out.fields |= kFieldAirGround;
  }
}

void set_ground_from_fs(DecodedMsg& out, unsigned fs) noexcept {
  // Flight status: 1/3 on ground; 0/2 "airborne" and 4/5 either. As in
  // readsb, only "on ground" is taken: an Address/Parity reply's address
  // comes from its CRC, and many transponders report "airborne" while
  // parked, so a reply that does not say "on ground" says nothing.
  if (fs == 1 || fs == 3) {
    out.on_ground = true;
    out.fields |= kFieldAirGround;
  }
}

double wrap_degrees(double deg) noexcept {
  double r = std::fmod(deg, 360.0);
  if (r < 0) r += 360.0;
  return r >= 360.0 ? 0.0 : r;
}

void decode_identification(const FrameBits& b, DecodedMsg& out) noexcept {
  out.category = u8(((0xEu - out.tc) << 4) | b.me(6, 8));
  out.fields |= kFieldCategory;
  bool valid = true;
  for (unsigned i = 0; i < 8; ++i) {
    const auto code = static_cast<unsigned>(b.me(9 + 6 * i, 14 + 6 * i));
    const char c = kCallsignChars[code];
    if (c == '#') valid = false;
    out.callsign[i] = c;
  }
  if (valid) out.fields |= kFieldCallsign;
}

void decode_cpr(const FrameBits& b, DecodedMsg& out, bool surface) noexcept {
  out.cpr.odd = b.me_bit(22) != 0;
  out.cpr.lat = static_cast<std::uint32_t>(b.me(23, 39));
  out.cpr.lon = static_cast<std::uint32_t>(b.me(40, 56));
  out.cpr_surface = surface;
  out.fields |= kFieldCpr;
}

void decode_surface_position(const FrameBits& b, DecodedMsg& out) noexcept {
  const double gs = surface_movement_to_knots(static_cast<unsigned>(b.me(6, 12)));
  if (gs >= 0.0) {
    out.ground_speed_kt = static_cast<float>(gs);
    out.fields |= kFieldGroundSpeed;
  }
  if (b.me_bit(13)) {
    out.track_deg = static_cast<float>(static_cast<double>(b.me(14, 20)) * 360.0 / 128.0);
    out.fields |= kFieldTrack;
  }
  out.on_ground = true;
  out.fields |= kFieldAirGround;
  decode_cpr(b, out, /*surface=*/true);
}

void decode_airborne_position(const FrameBits& b, DecodedMsg& out, bool gnss) noexcept {
  set_altitude(out, decode_ac12(u16(b.me(9, 20))), gnss);
  // For TIS-B / ADS-R (DF18 CF 2, 5, 6) bit 8 is the IMF flag instead.
  out.nic_supplement_b = (out.df == 17 || out.ca <= 1) && b.me_bit(8) != 0;
  decode_cpr(b, out, /*surface=*/false);
}

void decode_velocity(const FrameBits& b, DecodedMsg& out) noexcept {
  const auto st = static_cast<unsigned>(b.me(6, 8));
  out.subtype = u8(st);
  if (st < 1 || st > 4) return;  // reserved subtypes

  if (st == 1 || st == 2) {
    const auto v_ew = static_cast<unsigned>(b.me(15, 24));
    const auto v_ns = static_cast<unsigned>(b.me(26, 35));
    if (v_ew != 0 && v_ns != 0) {
      const double scale = st == 2 ? 4.0 : 1.0;  // subtype 2 = supersonic
      const double vx = (v_ew - 1.0) * scale * (b.me_bit(14) ? -1.0 : 1.0);  // + east
      const double vy = (v_ns - 1.0) * scale * (b.me_bit(25) ? -1.0 : 1.0);  // + north
      const double gs = std::hypot(vx, vy);
      out.ground_speed_kt = static_cast<float>(gs);
      out.fields |= kFieldGroundSpeed;
      if (gs > 0.0) {
        out.track_deg = static_cast<float>(
            wrap_degrees(std::atan2(vx, vy) * 180.0 / std::numbers::pi));
        out.fields |= kFieldTrack;
      }
    }
  } else {
    if (b.me_bit(14)) {
      out.heading_deg = static_cast<float>(static_cast<double>(b.me(15, 24)) * 360.0 / 1024.0);
      out.fields |= kFieldHeading;
    }
    const auto as = static_cast<unsigned>(b.me(26, 35));
    if (as != 0) {
      const double scale = st == 4 ? 4.0 : 1.0;
      out.airspeed_kt = static_cast<float>((as - 1.0) * scale);
      out.airspeed_true = b.me_bit(25) != 0;
      out.fields |= kFieldAirspeed;
    }
  }

  const auto vr = static_cast<std::int32_t>(b.me(38, 46));
  if (vr != 0) {
    out.vertical_rate_fpm = (vr - 1) * 64 * (b.me_bit(37) ? -1 : 1);
    out.vertical_rate_gnss = b.me_bit(36) == 0;  // VrSrc: 0 = GNSS, 1 = baro
    out.fields |= kFieldVerticalRate;
  }

  const auto diff = static_cast<std::int32_t>(b.me(50, 56));
  if (diff != 0) {
    out.gnss_baro_diff_ft = (diff - 1) * 25 * (b.me_bit(49) ? -1 : 1);
    out.fields |= kFieldGnssBaroDiff;
  }
}

void decode_aircraft_status(const FrameBits& b, DecodedMsg& out) noexcept {
  const auto st = static_cast<unsigned>(b.me(6, 8));
  out.subtype = u8(st);
  if (st != 1) return;  // only emergency/priority status is decoded
  out.emergency = u8(b.me(9, 11));
  out.fields |= kFieldEmergency;
  // An all-zero Mode A field here means "no code available" (unlike DF5/21,
  // where the identity field is always present and 0000 is a real squawk).
  const auto id13 = u16(b.me(12, 24));
  if (id13 != 0) {
    out.squawk = id13_to_mode_a(id13);
    out.fields |= kFieldSquawk;
  }
}

// TC 29 subtype 1 (DO-260B). ME bits: 8 SIL supplement, 9 altitude source,
// 10-20 selected altitude, 21-29 baro setting, 30 heading status, 31-39
// heading, 40-43 NACp, 44 NIC-baro, 45-46 SIL, 47 mode status, 48
// autopilot, 49 VNAV, 50 altitude hold, 51 reserved, 52 approach, 53 TCAS
// operational, 54 LNAV.
void decode_target_state(const FrameBits& b, DecodedMsg& out) noexcept {
  out.subtype = u8(b.me(6, 7));
  if (out.subtype != 1) return;  // subtype 0 (DO-260A) has another layout
  TargetState& ts = out.target;
  ts.sil_per_sample = b.me_bit(8) != 0;
  if (const auto alt = static_cast<std::int32_t>(b.me(10, 20)); alt != 0) {
    ts.selected_altitude_ft = (alt - 1) * 32;
    ts.altitude_from_fms = b.me_bit(9) != 0;
    ts.valid |= TargetState::kAltitude;
  }
  if (const auto baro = static_cast<unsigned>(b.me(21, 29)); baro != 0) {
    ts.baro_setting_hpa = static_cast<float>(800.0 + (baro - 1.0) * 0.8);
    ts.valid |= TargetState::kBaro;
  }
  if (b.me_bit(30)) {
    ts.selected_heading_deg = static_cast<float>(static_cast<double>(b.me(31, 39)) * 180.0 / 256.0);
    ts.valid |= TargetState::kHeading;
  }
  ts.nac_p = u8(b.me(40, 43));
  ts.nic_baro = b.me_bit(44) != 0;
  ts.sil = u8(b.me(45, 46));
  if (b.me_bit(47)) {
    ts.autopilot = b.me_bit(48) != 0;
    ts.vnav = b.me_bit(49) != 0;
    ts.altitude_hold = b.me_bit(50) != 0;
    ts.approach = b.me_bit(52) != 0;
    ts.lnav = b.me_bit(54) != 0;
    ts.valid |= TargetState::kModes;
  }
  ts.tcas_operational = b.me_bit(53) != 0;
  out.fields |= kFieldTargetState;
}

// TC 31. ME bits: 6-8 subtype (0 airborne, 1 surface); capability class
// 9-24 (surface 9-20, then length/width 21-24), operational mode 25-40,
// version 41-43, and from version 1: 44 NIC supplement-A, 45-48 NACp, 49-50
// GVA (version 2, airborne), 51-52 SIL, 53 NIC-baro (airborne), 54 HRD, 55
// SIL supplement (version 2).
void decode_operational_status(const FrameBits& b, DecodedMsg& out) noexcept {
  out.subtype = u8(b.me(6, 8));
  if (out.subtype > 1) return;  // reserved
  OperationalStatus& os = out.op_status;
  os.surface = out.subtype == 1;
  os.capability = u16(os.surface ? b.me(9, 20) : b.me(9, 24));
  os.operational_mode = u16(b.me(25, 40));
  os.version = u8(b.me(41, 43));
  // Versions 3-7 are reserved: the rest of the field is not read (readsb
  // decodes versions 1 and 2 only).
  if (os.version == 1 || os.version == 2) {
    os.nic_supplement_a = b.me_bit(44) != 0;
    os.nac_p = u8(b.me(45, 48));
    os.sil = u8(b.me(51, 52));
    os.heading_true_north = b.me_bit(54) == 0;  // HRD: 0 true north, 1 magnetic
    if (!os.surface) os.nic_baro = b.me_bit(53) != 0;
    if (os.version == 2) {
      os.sil_per_sample = b.me_bit(55) != 0;
      // NIC-C is capability class bit 20 when the class code (bits 9-10) is 0.
      if (os.surface && b.me(9, 10) == 0) {
        os.nic_supplement_c = b.me_bit(20) != 0;
        os.nic_supplement_c_valid = true;
      }
      if (!os.surface) os.gva = u8(b.me(49, 50));
    }
  }
  out.fields |= kFieldOpStatus;
}

void decode_me(const FrameBits& b, DecodedMsg& out) noexcept {
  out.tc = u8(b.me(1, 5));
  const unsigned tc = out.tc;
  if (tc == 0) {
    // No position information, but the altitude field may be filled in.
    set_altitude(out, decode_ac12(u16(b.me(9, 20))), false);
  } else if (tc <= 4) {
    decode_identification(b, out);
  } else if (tc <= 8) {
    decode_surface_position(b, out);
  } else if (tc <= 18) {
    decode_airborne_position(b, out, /*gnss=*/false);
  } else if (tc == 19) {
    decode_velocity(b, out);
  } else if (tc <= 22) {
    decode_airborne_position(b, out, /*gnss=*/true);
  } else if (tc == 28) {
    decode_aircraft_status(b, out);
  } else if (tc == 29) {
    decode_target_state(b, out);
  } else if (tc == 31) {
    decode_operational_status(b, out);
  }
  // Other type codes: frame is valid; no fields decoded yet.
}

// ---------------------------------------------------------------------------
// Comm-B (ICAO Doc 9871 register formats). MB bits are 1-indexed, 1..56.

class MbBits {
 public:
  explicit MbBits(const std::uint8_t* mb) noexcept {
    for (int i = 0; i < 7; ++i) v_ = (v_ << 8) | mb[i];
  }
  [[nodiscard]] std::uint64_t get(unsigned first, unsigned last) const noexcept {
    return (v_ >> (56 - last)) & ((std::uint64_t{1} << (last - first + 1)) - 1);
  }
  [[nodiscard]] bool bit(unsigned n) const noexcept { return get(n, n) != 0; }
  // A field's status bit is clear but its value bits are not: not this register.
  [[nodiscard]] bool wrong_status(unsigned status, unsigned first, unsigned last) const noexcept {
    return !bit(status) && get(first, last) != 0;
  }
  // Two's complement value of [first, last] with the sign bit just before it.
  [[nodiscard]] double signed_value(unsigned sign, unsigned first, unsigned last) const noexcept {
    const auto v = static_cast<double>(get(first, last));
    return bit(sign) ? v - static_cast<double>(std::uint64_t{1} << (last - first + 1)) : v;
  }
  [[nodiscard]] std::uint64_t raw() const noexcept { return v_; }

 private:
  std::uint64_t v_ = 0;
};

bool callsign_from(const MbBits& m, unsigned first, char* out) noexcept {
  bool valid = true;
  for (unsigned i = 0; i < 8; ++i) {
    const char c = kCallsignChars[m.get(first + 6 * i, first + 5 + 6 * i)];
    if (c == '#') valid = false;
    out[i] = c;
  }
  return valid;
}

bool is_bds10(const MbBits& m) noexcept {
  return m.get(1, 8) == 0x10 && m.get(10, 14) == 0;  // reserved bits
}

bool is_bds17(const MbBits& m) noexcept {
  // Capability bits 1-24, bits 25-56 reserved; every transponder that
  // answers GICB supports 2,0 (bit 7).
  return m.get(25, 56) == 0 && m.bit(7);
}

bool is_bds20(const MbBits& m) noexcept {
  char cs[8];
  return m.get(1, 8) == 0x20 && callsign_from(m, 9, cs);
}

bool is_bds30(const MbBits& m) noexcept {
  // Threat type 3 is invalid; RAC values from 48 up are reserved for ACAS III.
  return m.get(1, 8) == 0x30 && m.get(29, 30) != 3 && m.get(16, 22) < 48;
}

// 4,0: 1 status, 2-13 MCP/FCU selected altitude (16 ft); 14, 15-26 FMS
// selected altitude; 27, 28-39 baro setting (0.1 hPa above 800); 40-47
// reserved; 48 status, 49-51 VNAV / altitude hold / approach; 52-53
// reserved; 54 status, 55-56 target altitude source.
bool is_bds40(const MbBits& m) noexcept {
  if (m.wrong_status(1, 2, 13) || m.wrong_status(14, 15, 26) || m.wrong_status(27, 28, 39)) return false;
  if (m.wrong_status(48, 49, 51) || m.wrong_status(54, 55, 56)) return false;
  if (m.get(40, 47) != 0 || m.get(52, 53) != 0) return false;
  if (!(m.bit(1) || m.bit(14) || m.bit(27))) return false;  // no data at all
  if (m.bit(1) && m.get(2, 13) * 16 > 50000) return false;
  if (m.bit(14) && m.get(15, 26) * 16 > 50000) return false;
  if (m.bit(27)) {
    const double baro = static_cast<double>(m.get(28, 39)) * 0.1 + 800.0;
    if (baro < 850.0 || baro > 1100.0) return false;
  }
  return true;
}

// 5,0: 1, 2 sign, 3-11 roll (45/256 deg); 12, 13 sign, 14-23 true track
// (90/512 deg); 24, 25-34 ground speed (2 kt); 35, 36 sign, 37-45 track
// rate (8/256 deg/s); 46, 47-56 true airspeed (2 kt).
bool is_bds50(const MbBits& m) noexcept {
  if (m.wrong_status(1, 2, 11) || m.wrong_status(12, 13, 23) || m.wrong_status(24, 25, 34) ||
      m.wrong_status(35, 36, 45) || m.wrong_status(46, 47, 56)) {
    return false;
  }
  if (!(m.bit(1) || m.bit(12) || m.bit(24) || m.bit(35) || m.bit(46))) return false;
  if (m.bit(1) && std::fabs(m.signed_value(2, 3, 11) * 45.0 / 256.0) > 50.0) return false;
  const bool gs = m.bit(24), tas = m.bit(46);
  if (gs && m.get(25, 34) * 2 > 600) return false;
  if (tas && m.get(47, 56) * 2 > 600) return false;
  if (gs && tas) {
    const auto d = static_cast<std::int64_t>(m.get(25, 34) * 2) - static_cast<std::int64_t>(m.get(47, 56) * 2);
    if (d > 200 || d < -200) return false;  // wind is not that strong
  }
  return true;
}

// (A 4,0 that carries only mode bits or the target-altitude source, without
// an altitude or baro setting, is not recognised: rare, and otherwise an
// MB field of a few stray bits would read as 4,0.)
// 6,0: 1, 2 sign, 3-12 magnetic heading (90/512 deg); 13, 14-23 IAS (kt);
// 24, 25-34 Mach (2.048/512); 35, 36 sign, 37-45 baro vertical rate (32
// ft/min); 46, 47 sign, 48-56 inertial vertical rate.
bool is_bds60(const MbBits& m) noexcept {
  if (m.wrong_status(1, 2, 12) || m.wrong_status(13, 14, 23) || m.wrong_status(24, 25, 34) ||
      m.wrong_status(35, 36, 45) || m.wrong_status(46, 47, 56)) {
    return false;
  }
  if (!(m.bit(1) || m.bit(13) || m.bit(24) || m.bit(35) || m.bit(46))) return false;
  if (m.bit(13) && m.get(14, 23) > 500) return false;
  if (m.bit(24) && static_cast<double>(m.get(25, 34)) * 2.048 / 512.0 > 1.0) return false;
  // Vertical rates beyond 6,000 ft/min (as pyModeS). Baro and inertial
  // rates are not compared: they legitimately differ in transients, and
  // rejecting a real 6,0 then often leaves 5,0 as the only candidate.
  if (m.bit(35) && std::fabs(m.signed_value(36, 37, 45) * 32.0) > 6000.0) return false;
  if (m.bit(46) && std::fabs(m.signed_value(47, 48, 56) * 32.0) > 6000.0) return false;
  return true;
}

double angle_360(double deg) noexcept { return deg < 0 ? deg + 360.0 : deg; }

// Syndrome of each single-bit error in a 112-bit frame (the CRC is linear).
constexpr std::array<std::uint32_t, 112> kSingleBit112 = [] {
  std::array<std::uint32_t, 112> t{};
  for (int i = 0; i < 112; ++i) {
    std::uint8_t f[14] = {};
    f[i / 8] = static_cast<std::uint8_t>(0x80u >> (i % 8));
    std::uint32_t crc = 0;
    for (int k = 0; k < 11; ++k) {
      crc = ((crc << 8) ^ detail::kCrc24Table[((crc >> 16) ^ f[k]) & 0xFFu]) & 0xFFFFFFu;
    }
    t[static_cast<std::size_t>(i)] = crc ^ (std::uint32_t{f[11]} << 16 | std::uint32_t{f[12]} << 8 | f[13]);
  }
  return t;
}();

}  // namespace

void decode_comm_b(const std::uint8_t* mb, CommB& out) noexcept {
  out = CommB{};
  const MbBits m(mb);
  if (m.raw() == 0) return;  // no data
  if (is_bds10(m)) out.candidates |= CommB::kBds10;
  if (is_bds17(m)) out.candidates |= CommB::kBds17;
  if (is_bds20(m)) out.candidates |= CommB::kBds20;
  if (is_bds30(m)) out.candidates |= CommB::kBds30;
  if (is_bds40(m)) out.candidates |= CommB::kBds40;
  if (is_bds50(m)) out.candidates |= CommB::kBds50;
  if (is_bds60(m)) out.candidates |= CommB::kBds60;
  switch (out.candidates) {
    case CommB::kBds10: out.bds = 0x10; break;
    case CommB::kBds17: out.bds = 0x17; break;
    case CommB::kBds20:
      out.bds = 0x20;
      (void)callsign_from(m, 9, out.callsign);
      out.valid |= CommB::kCallsign;
      break;
    case CommB::kBds30: out.bds = 0x30; break;
    case CommB::kBds40:
      out.bds = 0x40;
      if (m.bit(1)) {
        out.mcp_altitude_ft = static_cast<std::int32_t>(m.get(2, 13) * 16);
        out.valid |= CommB::kMcpAltitude;
      }
      if (m.bit(14)) {
        out.fms_altitude_ft = static_cast<std::int32_t>(m.get(15, 26) * 16);
        out.valid |= CommB::kFmsAltitude;
      }
      if (m.bit(27)) {
        out.baro_setting_hpa = static_cast<float>(static_cast<double>(m.get(28, 39)) * 0.1 + 800.0);
        out.valid |= CommB::kBaroSetting;
      }
      break;
    case CommB::kBds50:
      out.bds = 0x50;
      if (m.bit(1)) {
        out.roll_deg = static_cast<float>(m.signed_value(2, 3, 11) * 45.0 / 256.0);
        out.valid |= CommB::kRoll;
      }
      if (m.bit(12)) {
        out.true_track_deg = static_cast<float>(angle_360(m.signed_value(13, 14, 23) * 90.0 / 512.0));
        out.valid |= CommB::kTrueTrack;
      }
      if (m.bit(24)) {
        out.ground_speed_kt = static_cast<float>(m.get(25, 34) * 2);
        out.valid |= CommB::kGroundSpeed;
      }
      if (m.bit(35)) {
        out.track_rate_deg_s = static_cast<float>(m.signed_value(36, 37, 45) * 8.0 / 256.0);
        out.valid |= CommB::kTrackRate;
      }
      if (m.bit(46)) {
        out.true_airspeed_kt = static_cast<float>(m.get(47, 56) * 2);
        out.valid |= CommB::kTrueAirspeed;
      }
      break;
    case CommB::kBds60:
      out.bds = 0x60;
      if (m.bit(1)) {
        out.magnetic_heading_deg = static_cast<float>(angle_360(m.signed_value(2, 3, 12) * 90.0 / 512.0));
        out.valid |= CommB::kMagneticHeading;
      }
      if (m.bit(13)) {
        out.indicated_airspeed_kt = static_cast<float>(m.get(14, 23));
        out.valid |= CommB::kIndicatedAirspeed;
      }
      if (m.bit(24)) {
        out.mach = static_cast<float>(static_cast<double>(m.get(25, 34)) * 2.048 / 512.0);
        out.valid |= CommB::kMach;
      }
      if (m.bit(35)) {
        out.baro_vertical_rate_fpm = static_cast<std::int32_t>(m.signed_value(36, 37, 45) * 32.0);
        out.valid |= CommB::kBaroVerticalRate;
      }
      if (m.bit(46)) {
        out.inertial_vertical_rate_fpm = static_cast<std::int32_t>(m.signed_value(47, 48, 56) * 32.0);
        out.valid |= CommB::kInertialVerticalRate;
      }
      break;
    default: break;  // none, or ambiguous
  }
}

const char* to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::kOk: return "ok";
    case DecodeStatus::kAddressParity: return "address-parity";
    case DecodeStatus::kBadCrc: return "bad-crc";
    case DecodeStatus::kBadLength: return "bad-length";
    case DecodeStatus::kUnknownFormat: return "unknown-format";
  }
  return "?";
}

std::size_t expected_frame_length(std::uint8_t first_byte) noexcept {
  const unsigned df = first_byte >> 3;
  if (df >= 24) return 14;  // DF24..31: Comm-D (first two bits "11")
  switch (df) {
    case 0: case 4: case 5: case 11:
      return 7;
    case 16: case 17: case 18: case 19: case 20: case 21:
      return 14;
    default:
      return 0;
  }
}

double surface_movement_to_knots(unsigned m) noexcept {
  if (m == 0 || m > 124) return -1.0;    // no information / reserved
  if (m == 1) return 0.0;                 // stopped (< 0.125 kt)
  if (m <= 8) return 0.125 * (m - 1);     // 0.125 kt steps
  if (m <= 12) return 1.0 + 0.25 * (m - 9);
  if (m <= 38) return 2.0 + 0.5 * (m - 13);
  if (m <= 93) return 15.0 + 1.0 * (m - 39);
  if (m <= 108) return 70.0 + 2.0 * (m - 94);
  if (m <= 123) return 100.0 + 5.0 * (m - 109);
  return 175.0;                           // 124: >= 175 kt
}

DecodeStatus decode(const std::uint8_t* frame, std::size_t len, DecodedMsg& out, const DecodeOptions& options) noexcept {
  out = DecodedMsg{};
  if (frame == nullptr || (len != 7 && len != 14)) return DecodeStatus::kBadLength;

  const unsigned raw_df = frame[0] >> 3;
  out.df = u8(raw_df >= 24 ? 24 : raw_df);
  const std::size_t expected = expected_frame_length(frame[0]);
  if (expected == 0 || out.df == 19) return DecodeStatus::kUnknownFormat;
  if (len != expected) return DecodeStatus::kBadLength;

  const FrameBits b(frame, len);
  out.syndrome = crc24_syndrome(frame, len);

  switch (out.df) {
    case 17:
    case 18: {
      if (out.syndrome != 0) {
        if (!options.fix_single_bit) return DecodeStatus::kBadCrc;
        // One wrong bit outside the DF field (a flipped DF bit would make it
        // another format): repair and decode the repaired frame.
        for (std::size_t i = 5; i < kSingleBit112.size(); ++i) {
          if (kSingleBit112[i] != out.syndrome) continue;
          std::uint8_t fixed[14];
          std::memcpy(fixed, frame, 14);
          fixed[i / 8] = u8(fixed[i / 8] ^ (0x80u >> (i % 8)));
          const std::uint32_t syndrome = out.syndrome;
          const DecodeStatus st = decode(fixed, 14, out, DecodeOptions{});
          out.corrected_bit = static_cast<std::int8_t>(i);
          out.syndrome = syndrome;  // the received frame's (diagnostics)
          return st;
        }
        return DecodeStatus::kBadCrc;
      }
      out.ca = u8(b.get(6, 8));
      out.icao = static_cast<std::uint32_t>(b.get(9, 32));
      if (out.df == 17) {
        set_ground_from_ca(out, out.ca);
        decode_me(b, out);
      } else {
        // CF: 0 ADS-B (ICAO), 1 ADS-B (non-ICAO), 2 fine TIS-B,
        // 5 TIS-B/ADS-R (non-ICAO), 6 ADS-R. Others use different formats.
        const unsigned cf = out.ca;
        out.non_icao_address = (cf == 1 || cf == 5);
        if (cf == 0 || cf == 1 || cf == 2 || cf == 5 || cf == 6) decode_me(b, out);
        // Fine TIS-B and ADS-R carry an IMF bit: 1 = the AA field is a ground
        // station track-file number, not an ICAO address. It sits at ME bit 8
        // in airborne position messages and ME bit 9 in velocity messages.
        // (Not checked for other type codes yet.)
        if (cf == 2 || cf == 6) {
          const unsigned tc = out.tc;
          if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
            out.non_icao_address = out.non_icao_address || b.me_bit(8) != 0;
          } else if (tc == 19) {
            out.non_icao_address = out.non_icao_address || b.me_bit(9) != 0;
          }
        }
      }
      return DecodeStatus::kOk;
    }
    case 11: {
      // Parity is overlaid with the interrogator's II/SI code (low 7 bits).
      if ((out.syndrome & ~0x7Fu) != 0) return DecodeStatus::kBadCrc;
      out.ca = u8(b.get(6, 8));
      out.icao = static_cast<std::uint32_t>(b.get(9, 32));
      set_ground_from_ca(out, out.ca);
      return DecodeStatus::kOk;
    }
    case 0:
    case 16: {
      out.icao = out.syndrome;
      if (b.bit(6) != 0) {  // VS: 1 = on ground; 0 is not taken as airborne (see set_ground_from_fs)
        out.on_ground = true;
        out.fields |= kFieldAirGround;
      }
      set_altitude(out, decode_ac13(u16(b.get(20, 32))), false);
      return DecodeStatus::kAddressParity;
    }
    case 4:
    case 20: {
      out.icao = out.syndrome;
      out.ca = u8(b.get(6, 8));
      set_ground_from_fs(out, out.ca);
      set_altitude(out, decode_ac13(u16(b.get(20, 32))), false);
      if (out.df == 20) {
        decode_comm_b(frame + 4, out.comm_b);
        out.fields |= kFieldCommB;
      }
      return DecodeStatus::kAddressParity;
    }
    case 5:
    case 21: {
      out.icao = out.syndrome;
      out.ca = u8(b.get(6, 8));
      set_ground_from_fs(out, out.ca);
      const auto id13 = u16(b.get(20, 32));
      out.squawk = id13_to_mode_a(id13);
      out.fields |= kFieldSquawk;
      if (out.df == 21) {
        decode_comm_b(frame + 4, out.comm_b);
        out.fields |= kFieldCommB;
      }
      return DecodeStatus::kAddressParity;
    }
    case 24: {
      out.icao = out.syndrome;
      return DecodeStatus::kAddressParity;
    }
    default:
      return DecodeStatus::kUnknownFormat;
  }
}

std::uint8_t position_nuc(unsigned tc) noexcept {
  static constexpr std::uint8_t kSurface[4] = {9, 8, 7, 6};  // TC 5-8
  static constexpr std::uint8_t kGnss[3] = {9, 8, 0};         // TC 20-22
  if (tc >= 5 && tc <= 8) return kSurface[tc - 5];
  if (tc >= 9 && tc <= 18) return static_cast<std::uint8_t>(18 - tc);
  if (tc >= 20 && tc <= 22) return kGnss[tc - 20];
  return 0;
}

std::uint8_t position_nic(unsigned tc, unsigned version, bool nic_a, bool nic_b, bool nic_c) noexcept {
  switch (tc) {
    case 5: case 9: case 20: return 11;
    case 6: case 10: case 21: return 10;
    case 7:  // surface
      if (version == 2) return nic_a && !nic_c ? 9 : 8;
      if (version == 1) return nic_a ? 9 : 8;
      return 8;
    case 8:  // surface
      if (version == 2) {
        if (nic_a && nic_c) return 7;
        if (nic_a != nic_c) return 6;
      }
      return 0;
    case 11:
      if (version == 2) return nic_a && nic_b ? 9 : 8;
      if (version == 1) return nic_a ? 9 : 8;
      return 8;
    case 12: return 7;
    case 13: return 6;
    case 14: return 5;
    case 15: return 4;
    case 16: return nic_a && nic_b ? 3 : 2;
    case 17: return 1;
    default: return 0;
  }
}

}  // namespace adsb::modes
