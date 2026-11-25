#include "adsb/modes/encoder.hpp"

#include <algorithm>
#include <cmath>

#include "adsb/modes/altitude.hpp"
#include "adsb/modes/crc24.hpp"

namespace adsb::modes {

namespace {

// Builds the 56-bit ME field with 1-indexed bit positions, as in the ICAO
// documents (ME bit 1 is the most significant).
class MeBuilder {
 public:
  void set(unsigned first, unsigned last, std::uint64_t value) noexcept {
    const unsigned width = last - first + 1;
    const std::uint64_t mask = (std::uint64_t{1} << width) - 1;
    me_ |= (value & mask) << (56 - last);
  }
  void write_to(std::uint8_t* frame) const noexcept {
    for (unsigned i = 0; i < 7; ++i) {
      frame[4 + i] = static_cast<std::uint8_t>(me_ >> (48 - 8 * i));
    }
  }

 private:
  std::uint64_t me_ = 0;
};

unsigned char_code(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return static_cast<unsigned>(c - 'A' + 1);
  if (c >= 'a' && c <= 'z') return static_cast<unsigned>(c - 'a' + 1);
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0' + 48);
  return 32;  // space (also for unsupported characters)
}

Frame df17(std::uint32_t icao, unsigned ca, const MeBuilder& me) noexcept {
  Frame f;
  f.len = 14;
  f.bytes[0] = static_cast<std::uint8_t>((17u << 3) | (ca & 7u));
  f.bytes[1] = static_cast<std::uint8_t>(icao >> 16);
  f.bytes[2] = static_cast<std::uint8_t>(icao >> 8);
  f.bytes[3] = static_cast<std::uint8_t>(icao);
  me.write_to(f.bytes);
  write_parity(f.bytes, 14, 0);
  return f;
}

Frame short_ap(unsigned df, unsigned fs, std::uint16_t field13, std::uint32_t icao) noexcept {
  Frame f;
  f.len = 7;
  f.bytes[0] = static_cast<std::uint8_t>((df << 3) | (fs & 7u));
  // Bits 9-19: DR (5) + UM (6) = 0. Bits 20-32: the 13-bit field.
  f.bytes[2] = static_cast<std::uint8_t>((field13 >> 8) & 0x1Fu);
  f.bytes[3] = static_cast<std::uint8_t>(field13 & 0xFFu);
  write_parity(f.bytes, 7, icao & 0xFFFFFFu);
  return f;
}

}  // namespace

void write_parity(std::uint8_t* frame, std::size_t len, std::uint32_t overlay) noexcept {
  const std::uint32_t p = crc24(frame, len - 3) ^ (overlay & 0xFFFFFFu);
  frame[len - 3] = static_cast<std::uint8_t>(p >> 16);
  frame[len - 2] = static_cast<std::uint8_t>(p >> 8);
  frame[len - 1] = static_cast<std::uint8_t>(p);
}

Frame encode_identification(std::uint32_t icao, const char callsign[8], std::uint8_t category,
                            unsigned ca) noexcept {
  // category 0xSC: S = 0xA..0xD -> TC 4..1, C = 0..7.
  const unsigned set = category >> 4;
  const unsigned tc = (set >= 0xA && set <= 0xD) ? 0xEu - set : 4u;
  MeBuilder me;
  me.set(1, 5, tc);
  me.set(6, 8, category & 7u);
  for (unsigned i = 0; i < 8; ++i) me.set(9 + 6 * i, 14 + 6 * i, char_code(callsign[i]));
  return df17(icao, ca, me);
}

Frame encode_airborne_position(std::uint32_t icao, std::int32_t alt_ft, const CprFrame& cpr,
                               unsigned tc, unsigned ca, bool nic_b) noexcept {
  MeBuilder me;
  me.set(1, 5, tc);
  me.set(8, 8, nic_b ? 1u : 0u);
  me.set(9, 20, encode_ac12(alt_ft).value_or(0));
  me.set(22, 22, cpr.odd ? 1u : 0u);
  me.set(23, 39, cpr.lat);
  me.set(40, 56, cpr.lon);
  return df17(icao, ca, me);
}

Frame encode_surface_position(std::uint32_t icao, double ground_speed_kt, double track_deg,
                              const CprFrame& cpr, unsigned tc, unsigned ca) noexcept {
  MeBuilder me;
  me.set(1, 5, tc);
  me.set(6, 12, knots_to_surface_movement(ground_speed_kt));
  if (track_deg >= 0.0) {
    me.set(13, 13, 1);
    const auto t = static_cast<unsigned>(std::lround(track_deg * 128.0 / 360.0)) & 0x7Fu;
    me.set(14, 20, t);
  }
  me.set(22, 22, cpr.odd ? 1u : 0u);
  me.set(23, 39, cpr.lat);
  me.set(40, 56, cpr.lon);
  return df17(icao, ca, me);
}

Frame encode_velocity(std::uint32_t icao, double east_kt, double north_kt,
                      std::int32_t vertical_rate_fpm, bool vr_baro, unsigned ca) noexcept {
  auto component = [](double v) {
    const long mag = std::lround(std::fabs(v));
    return static_cast<unsigned>(mag > 1021 ? 1022 : mag + 1);  // 0 means "no info"
  };
  MeBuilder me;
  me.set(1, 5, 19);
  me.set(6, 8, 1);  // subtype 1: ground speed, subsonic
  me.set(14, 14, east_kt < 0 ? 1u : 0u);
  me.set(15, 24, component(east_kt));
  me.set(25, 25, north_kt < 0 ? 1u : 0u);
  me.set(26, 35, component(north_kt));
  me.set(36, 36, vr_baro ? 1u : 0u);
  me.set(37, 37, vertical_rate_fpm < 0 ? 1u : 0u);
  long vr = std::lround(std::fabs(static_cast<double>(vertical_rate_fpm)) / 64.0);
  if (vr > 510) vr = 510;
  me.set(38, 46, static_cast<std::uint64_t>(vr + 1));
  return df17(icao, ca, me);
}

Frame encode_airspeed_heading(std::uint32_t icao, double heading_deg, double airspeed_kt, bool tas,
                              std::int32_t vertical_rate_fpm, unsigned ca) noexcept {
  MeBuilder me;
  me.set(1, 5, 19);
  me.set(6, 8, 3);  // subtype 3: airspeed and heading, subsonic
  if (heading_deg >= 0.0) {
    me.set(14, 14, 1);
    me.set(15, 24, static_cast<std::uint64_t>(std::lround(heading_deg * 1024.0 / 360.0)) & 0x3FFu);
  }
  me.set(25, 25, tas ? 1u : 0u);
  const long as = std::lround(std::fabs(airspeed_kt));
  me.set(26, 35, static_cast<std::uint64_t>(as > 1021 ? 1022 : as + 1));
  me.set(36, 36, 1);  // baro vertical rate
  me.set(37, 37, vertical_rate_fpm < 0 ? 1u : 0u);
  long vr = std::lround(std::fabs(static_cast<double>(vertical_rate_fpm)) / 64.0);
  if (vr > 510) vr = 510;
  me.set(38, 46, static_cast<std::uint64_t>(vr + 1));
  return df17(icao, ca, me);
}

Frame encode_operational_status(std::uint32_t icao, const OperationalStatus& os, unsigned ca) noexcept {
  MeBuilder me;
  me.set(1, 5, 31);
  me.set(6, 8, os.surface ? 1u : 0u);
  if (os.surface) {
    std::uint64_t cc = os.capability & 0xFFFu;
    if (os.version == 2) cc = (cc & ~std::uint64_t{1}) | (os.nic_supplement_c ? 1u : 0u);
    me.set(9, 20, cc);
  } else {
    me.set(9, 24, os.capability);
  }
  me.set(25, 40, os.operational_mode);
  me.set(41, 43, os.version);
  if (os.version >= 1) {
    me.set(44, 44, os.nic_supplement_a ? 1u : 0u);
    me.set(45, 48, os.nac_p);
    me.set(51, 52, os.sil);
    if (!os.surface) me.set(53, 53, os.nic_baro ? 1u : 0u);
    me.set(54, 54, os.heading_true_north ? 0u : 1u);
    if (os.version >= 2) {
      if (!os.surface) me.set(49, 50, os.gva);
      me.set(55, 55, os.sil_per_sample ? 1u : 0u);
    }
  }
  return df17(icao, ca, me);
}

Frame encode_df11(std::uint32_t icao, unsigned ca, unsigned iid) noexcept {
  Frame f;
  f.len = 7;
  f.bytes[0] = static_cast<std::uint8_t>((11u << 3) | (ca & 7u));
  f.bytes[1] = static_cast<std::uint8_t>(icao >> 16);
  f.bytes[2] = static_cast<std::uint8_t>(icao >> 8);
  f.bytes[3] = static_cast<std::uint8_t>(icao);
  write_parity(f.bytes, 7, iid & 0x7Fu);
  return f;
}

Frame encode_df20(std::uint32_t icao, std::int32_t alt_ft, std::uint64_t mb, unsigned fs) noexcept {
  Frame f;
  f.len = 14;
  const std::uint16_t ac = encode_ac13(alt_ft).value_or(0);
  f.bytes[0] = static_cast<std::uint8_t>((20u << 3) | (fs & 7u));
  f.bytes[2] = static_cast<std::uint8_t>((ac >> 8) & 0x1Fu);
  f.bytes[3] = static_cast<std::uint8_t>(ac & 0xFFu);
  for (int i = 0; i < 7; ++i) f.bytes[4 + i] = static_cast<std::uint8_t>(mb >> (8 * (6 - i)));
  write_parity(f.bytes, 14, icao & 0xFFFFFFu);
  return f;
}

std::uint64_t comm_b_bds60(double heading_deg, double ias_kt, double mach, int baro_vr_fpm,
                           int inertial_vr_fpm) noexcept {
  std::uint64_t mb = 0;
  auto put = [&](int first, int last, std::uint64_t v) {  // MB bits, 1-based
    const int w = last - first + 1;
    mb |= (v & ((std::uint64_t{1} << w) - 1)) << (56 - last);
  };
  double h = std::fmod(heading_deg, 360.0);
  if (h < 0) h += 360.0;
  auto hc = static_cast<std::int64_t>(std::lround(h * 512.0 / 90.0));
  if (hc >= 1024) hc -= 2048;  // two's complement, sign in bit 2
  put(1, 1, 1);
  put(2, 12, static_cast<std::uint64_t>(hc) & 0x7FFu);
  put(13, 13, 1);
  put(14, 23, static_cast<std::uint64_t>(std::clamp<long>(std::lround(ias_kt), 0, 1023)));
  put(24, 24, 1);
  put(25, 34, static_cast<std::uint64_t>(std::clamp<long>(std::lround(mach / 0.004), 0, 1023)));
  auto vr = [&](int first, int fpm) {
    const auto c = static_cast<std::int64_t>(std::lround(fpm / 32.0));
    put(first, first, 1);
    put(first + 1, first + 10, static_cast<std::uint64_t>(c) & 0x3FFu);  // sign + 9 bits
  };
  vr(35, baro_vr_fpm);
  vr(46, inertial_vr_fpm);
  return mb;
}

Frame encode_df4(std::uint32_t icao, std::int32_t alt_ft, unsigned fs) noexcept {
  return short_ap(4, fs, encode_ac13(alt_ft).value_or(0), icao);
}

Frame encode_df5(std::uint32_t icao, std::uint16_t squawk, unsigned fs) noexcept {
  return short_ap(5, fs, mode_a_to_id13(squawk), icao);
}

unsigned knots_to_surface_movement(double kt) noexcept {
  if (kt < 0.0) return 0;  // no information
  if (kt < 0.0625) return 1;  // stopped
  // Piecewise-linear segments of the movement code (decoder.cpp inverse).
  struct Seg {
    unsigned first, last;
    double base, step;
  };
  static constexpr Seg kSegs[] = {
      {2, 8, 0.125, 0.125}, {9, 12, 1.0, 0.25},    {13, 38, 2.0, 0.5},
      {39, 93, 15.0, 1.0},  {94, 108, 70.0, 2.0},  {109, 123, 100.0, 5.0},
  };
  for (const Seg& s : kSegs) {
    const double top = s.base + s.step * (s.last - s.first);
    if (kt <= top + s.step / 2) {
      long k = std::lround((kt - s.base) / s.step);
      if (k < 0) k = 0;
      return s.first + static_cast<unsigned>(k);
    }
  }
  return 124;  // >= 175 kt
}

}  // namespace adsb::modes
