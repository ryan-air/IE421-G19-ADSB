#include "adsb/modes/decoder.hpp"

#include <cmath>
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
  // Flight status: 0/2 airborne, 1/3 on ground, 4/5 either (SPI / alert).
  if (fs <= 3) {
    out.on_ground = (fs & 1u) != 0;
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
  }
  // Other type codes: frame is valid; no fields decoded yet.
}

}  // namespace

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

DecodeStatus decode(const std::uint8_t* frame, std::size_t len, DecodedMsg& out) noexcept {
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
      if (out.syndrome != 0) return DecodeStatus::kBadCrc;
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
      out.on_ground = b.bit(6) != 0;  // VS: 1 = on ground
      out.fields |= kFieldAirGround;
      set_altitude(out, decode_ac13(u16(b.get(20, 32))), false);
      return DecodeStatus::kAddressParity;
    }
    case 4:
    case 20: {
      out.icao = out.syndrome;
      out.ca = u8(b.get(6, 8));
      set_ground_from_fs(out, out.ca);
      set_altitude(out, decode_ac13(u16(b.get(20, 32))), false);
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

}  // namespace adsb::modes
