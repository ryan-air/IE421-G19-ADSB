#include "adsb/modes/decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <initializer_list>
#include <string>
#include <tuple>
#include <vector>

#include "adsb/modes/crc24.hpp"
#include "test_util.hpp"

namespace {

using namespace adsb::modes;
using adsb::test::hex;

DecodedMsg decode_ok(const char* s) {
  const auto f = hex(s);
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk) << s;
  return m;
}

std::string callsign(const DecodedMsg& m) { return std::string(m.callsign, 8); }

// Rewrites the parity of a long frame so the CRC is valid again.
void fix_parity(std::vector<std::uint8_t>& f) {
  const std::uint32_t p = crc24(f.data(), f.size() - 3);
  f[f.size() - 3] = static_cast<std::uint8_t>(p >> 16);
  f[f.size() - 2] = static_cast<std::uint8_t>(p >> 8);
  f[f.size() - 1] = static_cast<std::uint8_t>(p);
}

// ---------------------------------------------------------------------------
// Published vectors ("The 1090 MHz Riddle", pyModeS documentation)
// ---------------------------------------------------------------------------

TEST(Decoder, IdentificationKlm1023) {
  const auto m = decode_ok("8D4840D6202CC371C32CE0576098");
  EXPECT_EQ(m.df, 17);
  EXPECT_EQ(m.ca, 5);
  EXPECT_EQ(m.icao, 0x4840D6u);
  EXPECT_EQ(m.tc, 4);
  EXPECT_TRUE(m.has(kFieldCallsign));
  EXPECT_EQ(callsign(m), "KLM1023 ");
  EXPECT_TRUE(m.has(kFieldCategory));
  EXPECT_EQ(m.category, 0xA0);  // TC 4 -> set A, CA 0
  EXPECT_TRUE(m.has(kFieldAirGround));
  EXPECT_FALSE(m.on_ground);    // CA 5 = airborne
}

TEST(Decoder, IdentificationEzy85mh) {
  const auto m = decode_ok("8D406B902015A678D4D220AA4BDA");
  EXPECT_EQ(m.icao, 0x406B90u);
  EXPECT_EQ(callsign(m), "EZY85MH ");
}

TEST(Decoder, AirbornePositionRiddle) {
  const auto even = decode_ok("8D40621D58C382D690C8AC2863A7");
  const auto odd = decode_ok("8D40621D58C386435CC412692AD6");
  for (const auto* m : {&even, &odd}) {
    EXPECT_EQ(m->icao, 0x40621Du);
    EXPECT_EQ(m->tc, 11);
    EXPECT_TRUE(m->has(kFieldAltitude));
    EXPECT_EQ(m->altitude_ft, 38000);
    EXPECT_FALSE(m->altitude_gnss);
    EXPECT_TRUE(m->has(kFieldCpr));
    EXPECT_FALSE(m->cpr_surface);
  }
  EXPECT_FALSE(even.cpr.odd);
  EXPECT_EQ(even.cpr.lat, 93000u);
  EXPECT_EQ(even.cpr.lon, 51372u);
  EXPECT_TRUE(odd.cpr.odd);
  EXPECT_EQ(odd.cpr.lat, 74158u);
  EXPECT_EQ(odd.cpr.lon, 50194u);

  const auto pos = cpr_global_airborne(even.cpr, odd.cpr, /*odd_is_newer=*/false);
  ASSERT_TRUE(pos.has_value());
  EXPECT_NEAR(pos->lat, 52.25720, 1e-5);
  EXPECT_NEAR(pos->lon, 3.91937, 1e-5);
}

TEST(Decoder, PyModeSPositionPair) {
  const auto even = decode_ok("8D40058B58C901375147EFD09357");
  const auto odd = decode_ok("8D40058B58C904A87F402D3B8C59");
  EXPECT_EQ(even.altitude_ft, 39000);
  ASSERT_FALSE(even.cpr.odd);
  ASSERT_TRUE(odd.cpr.odd);
  // Odd frame received 5 s after the even one.
  const auto pos = cpr_global_airborne(even.cpr, odd.cpr, /*odd_is_newer=*/true);
  ASSERT_TRUE(pos.has_value());
  EXPECT_NEAR(pos->lat, 49.81755, 1e-5);
  EXPECT_NEAR(pos->lon, 6.08442, 1e-5);
}

TEST(Decoder, VelocityGroundSpeed) {
  const auto m = decode_ok("8D485020994409940838175B284F");
  EXPECT_EQ(m.icao, 0x485020u);
  EXPECT_EQ(m.tc, 19);
  EXPECT_EQ(m.subtype, 1);
  ASSERT_TRUE(m.has(kFieldGroundSpeed));
  ASSERT_TRUE(m.has(kFieldTrack));
  EXPECT_NEAR(m.ground_speed_kt, 159.20, 0.01);
  EXPECT_NEAR(m.track_deg, 182.88, 0.01);
  ASSERT_TRUE(m.has(kFieldVerticalRate));
  EXPECT_EQ(m.vertical_rate_fpm, -832);
  EXPECT_TRUE(m.vertical_rate_gnss);  // VrSrc bit (ME 36) = 0 -> GNSS
  EXPECT_TRUE(m.has(kFieldGnssBaroDiff));
  EXPECT_EQ(m.gnss_baro_diff_ft, 550);
}

TEST(Decoder, VelocityAirspeed) {
  const auto m = decode_ok("8DA05F219B06B6AF189400CBC33F");
  EXPECT_EQ(m.icao, 0xA05F21u);
  EXPECT_EQ(m.subtype, 3);
  ASSERT_TRUE(m.has(kFieldHeading));
  EXPECT_NEAR(m.heading_deg, 243.98, 0.01);
  ASSERT_TRUE(m.has(kFieldAirspeed));
  EXPECT_NEAR(m.airspeed_kt, 375.0, 1e-6);
  EXPECT_TRUE(m.airspeed_true);
  ASSERT_TRUE(m.has(kFieldVerticalRate));
  EXPECT_EQ(m.vertical_rate_fpm, -2304);
  EXPECT_FALSE(m.has(kFieldGroundSpeed));
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(Decoder, RejectsCorruptedFrames) {
  auto f = hex("8D4840D6202CC371C32CE0576098");
  f[5] ^= 0x01;
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kBadCrc);
  EXPECT_EQ(m.fields, 0u) << "no fields may be reported for a bad frame";
}

TEST(Decoder, RejectsWrongLengths) {
  const auto f = hex("8D4840D6202CC371C32CE0576098");
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), 7, m), DecodeStatus::kBadLength) << "DF17 is a long format";
  EXPECT_EQ(decode(f.data(), 13, m), DecodeStatus::kBadLength);
  EXPECT_EQ(decode(f.data(), 0, m), DecodeStatus::kBadLength);
  EXPECT_EQ(decode(nullptr, 14, m), DecodeStatus::kBadLength);

  std::uint8_t df11_as_long[14] = {static_cast<std::uint8_t>(11u << 3)};
  EXPECT_EQ(decode(df11_as_long, 14, m), DecodeStatus::kBadLength);
}

TEST(Decoder, RejectsUnassignedFormats) {
  for (const unsigned df : {1u, 2u, 3u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u, 15u, 22u, 23u}) {
    std::uint8_t f[14] = {static_cast<std::uint8_t>(df << 3)};
    DecodedMsg m;
    EXPECT_EQ(decode(f, 7, m), DecodeStatus::kUnknownFormat) << "DF" << df;
    EXPECT_EQ(decode(f, 14, m), DecodeStatus::kUnknownFormat) << "DF" << df;
    EXPECT_EQ(expected_frame_length(f[0]), 0u) << "DF" << df;
  }
}

TEST(Decoder, FrameLengths) {
  for (const unsigned df : {0u, 4u, 5u, 11u}) {
    EXPECT_EQ(expected_frame_length(static_cast<std::uint8_t>(df << 3)), 7u) << df;
  }
  for (const unsigned df : {16u, 17u, 18u, 19u, 20u, 21u, 24u, 25u, 31u}) {
    EXPECT_EQ(expected_frame_length(static_cast<std::uint8_t>(df << 3)), 14u) << df;
  }
}

// ---------------------------------------------------------------------------
// Synthetic frames for formats without published vectors in this suite
// ---------------------------------------------------------------------------

TEST(Decoder, AddressParityFormatsRecoverAddress) {
  // DF4 (altitude), DF5 (identity), DF20, DF21: parity = CRC XOR address.
  struct Case {
    unsigned df;
    std::size_t len;
  };
  for (const Case c : {Case{4, 7}, Case{5, 7}, Case{20, 14}, Case{21, 14}, Case{0, 7}, Case{16, 14}}) {
    std::vector<std::uint8_t> f(c.len, 0);
    // DF4/5/20/21: FS (bits 6-8) = 1 -> on ground. DF0/16: VS (bit 6) = 1.
    const bool vs_format = (c.df == 0 || c.df == 16);
    f[0] = static_cast<std::uint8_t>((c.df << 3) | (vs_format ? 0x4 : 0x1));
    // AC/ID field (bits 20-32) = 0x0C38 with Q bit -> 38000 ft (for altitude DFs).
    const std::uint16_t ac13 = 0x1838;
    f[2] = static_cast<std::uint8_t>((ac13 >> 8) & 0x1F);
    f[3] = static_cast<std::uint8_t>(ac13 & 0xFF);
    const std::uint32_t icao = 0xABCDEF;
    const std::uint32_t ap = crc24(f.data(), c.len - 3) ^ icao;
    f[c.len - 3] = static_cast<std::uint8_t>(ap >> 16);
    f[c.len - 2] = static_cast<std::uint8_t>(ap >> 8);
    f[c.len - 1] = static_cast<std::uint8_t>(ap);

    DecodedMsg m;
    ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kAddressParity) << "DF" << c.df;
    EXPECT_EQ(m.icao, icao) << "DF" << c.df;
    EXPECT_EQ(m.df, c.df);
    if (c.df == 5 || c.df == 21) {
      EXPECT_TRUE(m.has(kFieldSquawk));
      EXPECT_FALSE(m.has(kFieldAltitude));
    } else {
      ASSERT_TRUE(m.has(kFieldAltitude)) << "DF" << c.df;
      EXPECT_EQ(m.altitude_ft, 38000);
    }
    ASSERT_TRUE(m.has(kFieldAirGround));
    EXPECT_TRUE(m.on_ground) << "DF" << c.df;
  }
}

TEST(Decoder, Df11AllCall) {
  std::uint8_t f[7] = {static_cast<std::uint8_t>((11u << 3) | 5u), 0x48, 0x40, 0xD6};
  // II = 0: parity is the plain CRC.
  std::uint32_t p = crc24(f, 4);
  f[4] = static_cast<std::uint8_t>(p >> 16);
  f[5] = static_cast<std::uint8_t>(p >> 8);
  f[6] = static_cast<std::uint8_t>(p);
  DecodedMsg m;
  ASSERT_EQ(decode(f, 7, m), DecodeStatus::kOk);
  EXPECT_EQ(m.icao, 0x4840D6u);
  EXPECT_EQ(m.ca, 5);

  // Nonzero II code in the low bits is accepted and visible in the syndrome.
  p ^= 0x0Au;
  f[6] = static_cast<std::uint8_t>(p);
  ASSERT_EQ(decode(f, 7, m), DecodeStatus::kOk);
  EXPECT_EQ(m.syndrome, 0x0Au);

  // The II/SI overlay is exactly the low 7 bits: 0x7F is accepted, 0x80 (the
  // first value outside it) and anything higher is a corrupted frame.
  const std::uint32_t clean = p ^ 0x0Au;
  for (const std::uint32_t overlay : {0x7Fu, 0x80u, 0x100u, 0x800u, 0x100000u}) {
    const std::uint32_t q = clean ^ overlay;
    f[4] = static_cast<std::uint8_t>(q >> 16);
    f[5] = static_cast<std::uint8_t>(q >> 8);
    f[6] = static_cast<std::uint8_t>(q);
    EXPECT_EQ(decode(f, 7, m), overlay <= 0x7Fu ? DecodeStatus::kOk : DecodeStatus::kBadCrc)
        << std::hex << overlay;
  }
}

TEST(Decoder, Df18NonIcaoAndUnsupportedCf) {
  auto f = hex("8D4840D6202CC371C32CE0576098");
  // DF18 CF=1 (non-ICAO address) carrying the same identification ME.
  f[0] = static_cast<std::uint8_t>((18u << 3) | 1u);
  fix_parity(f);
  DecodedMsg m;
  ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk);
  EXPECT_EQ(m.df, 18);
  EXPECT_TRUE(m.non_icao_address);
  EXPECT_EQ(callsign(m), "KLM1023 ");

  // CF=2 (fine TIS-B) / CF=6 (ADS-R): the IMF bit decides the address type.
  // Airborne position: IMF is ME bit 8 (frame bit 40 -> byte 4, mask 0x01).
  for (const unsigned cf : {2u, 6u}) {
    auto pos = hex("8D40621D58C382D690C8AC2863A7");
    pos[0] = static_cast<std::uint8_t>((18u << 3) | cf);
    pos[4] = static_cast<std::uint8_t>(pos[4] & ~0x01u);
    fix_parity(pos);
    ASSERT_EQ(decode(pos.data(), pos.size(), m), DecodeStatus::kOk);
    EXPECT_FALSE(m.non_icao_address) << "CF" << cf << " IMF=0";
    EXPECT_EQ(m.altitude_ft, 38000);
    pos[4] = static_cast<std::uint8_t>(pos[4] | 0x01u);
    fix_parity(pos);
    ASSERT_EQ(decode(pos.data(), pos.size(), m), DecodeStatus::kOk);
    EXPECT_TRUE(m.non_icao_address) << "CF" << cf << " IMF=1";
  }
  // Velocity: IMF is ME bit 9 (frame bit 41 -> byte 5, mask 0x80).
  auto vel = hex("8D485020994409940838175B284F");
  vel[0] = static_cast<std::uint8_t>((18u << 3) | 2u);
  vel[5] = static_cast<std::uint8_t>(vel[5] | 0x80u);
  fix_parity(vel);
  ASSERT_EQ(decode(vel.data(), vel.size(), m), DecodeStatus::kOk);
  EXPECT_TRUE(m.non_icao_address);
  EXPECT_NEAR(m.ground_speed_kt, 159.20, 0.01) << "IMF must not disturb velocity fields";

  // CF=3 (coarse TIS-B) uses a different ME layout: frame valid, no fields.
  f[0] = static_cast<std::uint8_t>((18u << 3) | 3u);
  fix_parity(f);
  ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk);
  EXPECT_EQ(m.fields, 0u);
}

TEST(Decoder, EmergencySquawk) {
  // DF17 TC28 subtype 1: emergency state 1 (general), squawk 7700.
  std::vector<std::uint8_t> f(14, 0);
  f[0] = 0x8D;
  f[1] = 0xAB; f[2] = 0xCD; f[3] = 0xEF;
  // ME bits 1-5 TC=28, 6-8 ST=1, 9-11 emergency=1, 12-24 ID13 for 7700.
  const std::uint64_t id13 = 0x800 | 0x200 | 0x080 | 0x020 | 0x008 | 0x002;
  const std::uint64_t me = (std::uint64_t{28} << 51) | (std::uint64_t{1} << 48) |
                           (std::uint64_t{1} << 45) | (id13 << 32);
  for (int i = 0; i < 7; ++i) f[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(me >> (48 - 8 * i));
  fix_parity(f);
  DecodedMsg m;
  ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk);
  EXPECT_EQ(m.tc, 28);
  EXPECT_EQ(m.subtype, 1);
  ASSERT_TRUE(m.has(kFieldEmergency));
  EXPECT_EQ(m.emergency, 1);
  ASSERT_TRUE(m.has(kFieldSquawk));
  EXPECT_EQ(m.squawk, 0x7700);
}

TEST(Decoder, SurfacePosition) {
  // TC 7 surface position with movement code 54 (30 kt) and track 64 (180 deg).
  std::vector<std::uint8_t> f(14, 0);
  f[0] = 0x8C;  // DF17, CA 4 (on ground)
  f[1] = 0x48; f[2] = 0x41; f[3] = 0x75;
  const CprFrame cpr = cpr_encode(52.32061, 4.73473, /*odd=*/false, /*surface=*/true);
  const std::uint64_t me = (std::uint64_t{7} << 51) | (std::uint64_t{54} << 44) |
                           (std::uint64_t{1} << 43) | (std::uint64_t{64} << 36) |
                           (std::uint64_t{cpr.lat} << 17) | cpr.lon;
  for (int i = 0; i < 7; ++i) f[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(me >> (48 - 8 * i));
  fix_parity(f);

  DecodedMsg m;
  ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk);
  EXPECT_EQ(m.tc, 7);
  ASSERT_TRUE(m.has(kFieldGroundSpeed));
  EXPECT_DOUBLE_EQ(m.ground_speed_kt, 30.0);
  ASSERT_TRUE(m.has(kFieldTrack));
  EXPECT_DOUBLE_EQ(m.track_deg, 180.0);
  EXPECT_TRUE(m.on_ground);
  ASSERT_TRUE(m.has(kFieldCpr));
  EXPECT_TRUE(m.cpr_surface);
  EXPECT_EQ(m.cpr.lat, cpr.lat);
  EXPECT_EQ(m.cpr.lon, cpr.lon);

  const LatLon p = cpr_local_surface(m.cpr, {51.990, 4.375});
  EXPECT_NEAR(p.lat, 52.32061, 2e-5);
  EXPECT_NEAR(p.lon, 4.73473, 2e-5);
}

TEST(Decoder, SurfaceMovementTable) {
  EXPECT_LT(surface_movement_to_knots(0), 0.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(1), 0.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(2), 0.125);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(8), 0.875);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(9), 1.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(12), 1.75);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(13), 2.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(38), 14.5);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(39), 15.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(93), 69.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(94), 70.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(108), 98.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(109), 100.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(123), 170.0);
  EXPECT_DOUBLE_EQ(surface_movement_to_knots(124), 175.0);
  EXPECT_LT(surface_movement_to_knots(125), 0.0);
  // Monotonic over the valid range.
  for (unsigned m = 2; m <= 124; ++m) {
    EXPECT_GT(surface_movement_to_knots(m), surface_movement_to_knots(m - 1)) << m;
  }
}

TEST(Decoder, InvalidCallsignCharactersAreFlagged) {
  auto f = hex("8D4840D6202CC371C32CE0576098");
  // Set the first 6-bit character to code 0 ('#', invalid): clear ME bits 9-14.
  f[5] = static_cast<std::uint8_t>(f[5] & 0x03);
  fix_parity(f);
  DecodedMsg m;
  ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk);
  EXPECT_FALSE(m.has(kFieldCallsign));
  EXPECT_EQ(m.callsign[0], '#');
}

TEST(Decoder, RandomValidFramesStayInPhysicalRanges) {
  // Random ME payloads with correct parity: whatever is decoded must be
  // physically meaningful (no NaN, angles in range, bounded speeds).
  std::mt19937_64 rng(5);
  int decoded = 0;
  for (int iter = 0; iter < 200'000; ++iter) {
    std::vector<std::uint8_t> f(14);
    for (auto& b : f) b = static_cast<std::uint8_t>(rng());
    f[0] = static_cast<std::uint8_t>((17u << 3) | (rng() & 7u));
    fix_parity(f);
    DecodedMsg m;
    ASSERT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kOk);
    ++decoded;
    if (m.has(kFieldAltitude)) {
      ASSERT_GE(m.altitude_ft, -1200);
      ASSERT_LE(m.altitude_ft, 126'750);
    }
    if (m.has(kFieldTrack)) {
      ASSERT_GE(m.track_deg, 0.0f);
      ASSERT_LT(m.track_deg, 360.0f);
    }
    if (m.has(kFieldHeading)) {
      ASSERT_GE(m.heading_deg, 0.0f);
      ASSERT_LT(m.heading_deg, 360.0f);
    }
    if (m.has(kFieldGroundSpeed)) {
      ASSERT_GE(m.ground_speed_kt, 0.0f);
      ASSERT_LE(m.ground_speed_kt, 6000.0f);  // supersonic subtype maximum ~5790 kt
    }
    if (m.has(kFieldCpr)) {
      ASSERT_LT(m.cpr.lat, 1u << 17);
      ASSERT_LT(m.cpr.lon, 1u << 17);
    }
  }
  EXPECT_EQ(decoded, 200'000);
}

// ---------------------------------------------------------------------------
// TC 29, TC 31, Comm-B, single-bit repair
// ---------------------------------------------------------------------------

// Sets frame bits [first, last] (1-indexed, inclusive) to v.
void set_bits(std::vector<std::uint8_t>& f, unsigned first, unsigned last, std::uint64_t v) {
  for (unsigned b = last + 1; b-- > first;) {
    const unsigned i = b - 1;
    const auto mask = static_cast<std::uint8_t>(0x80u >> (i % 8));
    f[i / 8] = static_cast<std::uint8_t>((v & 1u) ? (f[i / 8] | mask) : (f[i / 8] & ~mask));
    v >>= 1;
  }
}

// A DF17 frame from address 0xABCDEF with the given ME bits set (ME bit n
// is frame bit 32 + n), parity fixed.
std::vector<std::uint8_t> df17_with(std::initializer_list<std::tuple<unsigned, unsigned, std::uint64_t>> me) {
  std::vector<std::uint8_t> f(14, 0);
  set_bits(f, 1, 5, 17);
  set_bits(f, 6, 8, 5);
  set_bits(f, 9, 32, 0xABCDEF);
  for (const auto& [a, z, v] : me) set_bits(f, 32 + a, 32 + z, v);
  fix_parity(f);
  return f;
}

DecodedMsg decode_vec(const std::vector<std::uint8_t>& f, DecodeStatus want = DecodeStatus::kOk) {
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m), want);
  return m;
}

// pyModeS's target state and status example (CRC clean).
TEST(Decoder, TargetStateAndStatusPublished) {
  const auto m = decode_ok("8DA05629EA21485CBF3F8CADAEEB");
  EXPECT_EQ(m.icao, 0xA05629u);
  EXPECT_EQ(m.tc, 29);
  EXPECT_EQ(m.subtype, 1);
  ASSERT_TRUE(m.has(kFieldTargetState));
  const TargetState& t = m.target;
  EXPECT_EQ(t.valid, TargetState::kAltitude | TargetState::kBaro | TargetState::kHeading | TargetState::kModes);
  EXPECT_EQ(t.selected_altitude_ft, 16992);
  EXPECT_FALSE(t.altitude_from_fms);
  EXPECT_NEAR(t.baro_setting_hpa, 1012.8, 1e-3);
  EXPECT_NEAR(t.selected_heading_deg, 66.796875, 1e-4);
  EXPECT_TRUE(t.autopilot);
  EXPECT_TRUE(t.vnav);
  EXPECT_FALSE(t.altitude_hold);
  EXPECT_FALSE(t.approach);
  EXPECT_TRUE(t.lnav);
  EXPECT_TRUE(t.tcas_operational);
  EXPECT_EQ(t.nac_p, 9);
  EXPECT_TRUE(t.nic_baro);
  EXPECT_EQ(t.sil, 3);
  EXPECT_FALSE(t.sil_per_sample);
}

TEST(Decoder, TargetStateFieldsWithoutData) {
  // Zero altitude and baro fields, heading and mode status bits clear: no
  // values, but the frame still says what it can (TCAS, NACp, SIL).
  auto m = decode_vec(df17_with({{1, 5, 29}, {6, 7, 1}, {8, 8, 1}, {9, 9, 1}, {40, 43, 10}, {45, 46, 2}, {53, 53, 1},
                                 {48, 50, 7}}));
  ASSERT_TRUE(m.has(kFieldTargetState));
  EXPECT_EQ(m.target.valid, 0u);
  EXPECT_FALSE(m.target.autopilot);  // mode bits ignored without their status bit
  EXPECT_TRUE(m.target.tcas_operational);
  EXPECT_TRUE(m.target.sil_per_sample);
  EXPECT_EQ(m.target.nac_p, 10);
  EXPECT_EQ(m.target.sil, 2);
  // FMS source, lowest codes: 0 ft, 800 hPa, heading 0 and the top of the range.
  m = decode_vec(df17_with({{1, 5, 29}, {6, 7, 1}, {9, 9, 1}, {10, 20, 1}, {21, 29, 1}, {30, 30, 1}, {31, 39, 511}}));
  EXPECT_EQ(m.target.selected_altitude_ft, 0);
  EXPECT_TRUE(m.target.altitude_from_fms);
  EXPECT_NEAR(m.target.baro_setting_hpa, 800.0, 1e-4);
  EXPECT_NEAR(m.target.selected_heading_deg, 511 * 180.0 / 256.0, 1e-4);
  m = decode_vec(df17_with({{1, 5, 29}, {10, 20, 2047}, {21, 29, 511}}));  // subtype 0: DO-260A layout
  EXPECT_EQ(m.subtype, 0);
  EXPECT_FALSE(m.has(kFieldTargetState));
  EXPECT_EQ(m.target.valid, 0u);
  m = decode_vec(df17_with({{1, 5, 29}, {6, 7, 1}, {10, 20, 2047}}));
  EXPECT_EQ(m.target.selected_altitude_ft, 2046 * 32);
  // Each mode bit on its own (the published frame has several set at once).
  m = decode_vec(df17_with({{1, 5, 29}, {6, 7, 1}, {47, 47, 1}, {54, 54, 1}}));
  EXPECT_TRUE(m.target.lnav);
  EXPECT_FALSE(m.target.tcas_operational || m.target.autopilot || m.target.vnav || m.target.altitude_hold || m.target.approach);
  m = decode_vec(df17_with({{1, 5, 29}, {6, 7, 1}, {47, 47, 1}, {52, 52, 1}}));
  EXPECT_TRUE(m.target.approach);
  EXPECT_FALSE(m.target.lnav || m.target.tcas_operational || m.target.altitude_hold);
  m = decode_vec(df17_with({{1, 5, 29}, {6, 7, 1}, {47, 47, 1}, {50, 50, 1}}));
  EXPECT_TRUE(m.target.altitude_hold);
  EXPECT_FALSE(m.target.vnav || m.target.approach);
}

TEST(Decoder, OperationalStatusAirborneVersion2) {
  const auto m = decode_vec(df17_with({{1, 5, 31}, {6, 8, 0}, {9, 24, 0x002A}, {25, 40, 0x0300}, {41, 43, 2}, {44, 44, 1},
                                       {45, 48, 9}, {49, 50, 2}, {51, 52, 3}, {53, 53, 1}, {54, 54, 1}, {55, 55, 1}}));
  ASSERT_TRUE(m.has(kFieldOpStatus));
  const OperationalStatus& o = m.op_status;
  EXPECT_FALSE(o.surface);
  EXPECT_EQ(o.version, 2);
  EXPECT_EQ(o.capability, 0x002A);
  EXPECT_EQ(o.operational_mode, 0x0300);
  EXPECT_TRUE(o.nic_supplement_a);
  EXPECT_EQ(o.nac_p, 9);
  EXPECT_EQ(o.gva, 2);
  EXPECT_EQ(o.sil, 3);
  EXPECT_TRUE(o.nic_baro);
  EXPECT_FALSE(o.heading_true_north);  // HRD 1 = magnetic north
  EXPECT_TRUE(o.sil_per_sample);
}

TEST(Decoder, OperationalStatusSurfaceAndOlderVersions) {
  // Surface, version 2: 12 capability bits, no GVA or NIC-baro (bit 53 is
  // the track/heading flag there).
  auto m = decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {9, 24, 0xFFFF}, {41, 43, 2}, {45, 48, 8}, {49, 50, 3}, {53, 53, 1}}));
  ASSERT_TRUE(m.has(kFieldOpStatus));
  EXPECT_TRUE(m.op_status.surface);
  EXPECT_EQ(m.op_status.capability, 0x0FFF);
  EXPECT_EQ(m.op_status.nac_p, 8);
  EXPECT_EQ(m.op_status.gva, 0);
  EXPECT_FALSE(m.op_status.nic_baro);
  // Version 1: no SIL supplement or GVA (bits 49-50 are BAQ).
  m = decode_vec(df17_with({{1, 5, 31}, {41, 43, 1}, {45, 48, 7}, {49, 50, 3}, {55, 55, 1}}));
  EXPECT_EQ(m.op_status.version, 1);
  EXPECT_EQ(m.op_status.nac_p, 7);
  EXPECT_EQ(m.op_status.gva, 0);
  EXPECT_FALSE(m.op_status.sil_per_sample);
  // Version 0: the rest of the field means something else.
  m = decode_vec(df17_with({{1, 5, 31}, {41, 43, 0}, {45, 48, 7}, {51, 52, 3}}));
  EXPECT_EQ(m.op_status.version, 0);
  EXPECT_EQ(m.op_status.nac_p, 0);
  EXPECT_EQ(m.op_status.sil, 0);
  // Subtypes 2-7 are reserved.
  m = decode_vec(df17_with({{1, 5, 31}, {6, 8, 2}, {41, 43, 2}}));
  EXPECT_FALSE(m.has(kFieldOpStatus));
}

TEST(Decoder, NicSupplementsBAndC) {
  // B: ME bit 8 of airborne positions (baro and GNSS); not read elsewhere.
  for (const unsigned tc : {9u, 11u, 16u, 18u, 20u, 22u}) {
    EXPECT_TRUE(decode_vec(df17_with({{1, 5, tc}, {8, 8, 1}})).nic_supplement_b) << tc;
    EXPECT_FALSE(decode_vec(df17_with({{1, 5, tc}})).nic_supplement_b) << tc;
  }
  EXPECT_FALSE(decode_vec(df17_with({{1, 5, 7}, {8, 8, 1}})).nic_supplement_b) << "surface: bit 8 is movement";
  // DF18: bit 8 is NIC-B for ADS-B (CF 0/1), the IMF flag for TIS-B / ADS-R (CF 2, 5, 6).
  for (const unsigned cf : {0u, 1u, 2u, 5u, 6u}) {
    auto f = df17_with({{1, 5, 16}, {8, 8, 1}});
    set_bits(f, 1, 5, 18);
    set_bits(f, 6, 8, cf);
    fix_parity(f);
    EXPECT_EQ(decode_vec(f).nic_supplement_b, cf <= 1) << "CF " << cf;
  }
  // C: capability class bit 20 of a version 2 surface status only.
  EXPECT_TRUE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {20, 20, 1}, {41, 43, 2}})).op_status.nic_supplement_c);
  EXPECT_FALSE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {20, 20, 1}, {41, 43, 1}})).op_status.nic_supplement_c);
  EXPECT_FALSE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 0}, {20, 20, 1}, {41, 43, 2}})).op_status.nic_supplement_c);
  EXPECT_FALSE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {41, 43, 2}})).op_status.nic_supplement_c);
  // Only with capability class code 0 (bits 9-10), as readsb.
  EXPECT_FALSE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {9, 10, 1}, {20, 20, 1}, {41, 43, 2}})).op_status.nic_supplement_c);
  EXPECT_FALSE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {9, 10, 1}, {41, 43, 2}})).op_status.nic_supplement_c_valid);
  EXPECT_TRUE(decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {41, 43, 2}})).op_status.nic_supplement_c_valid);
}

TEST(Decoder, OperationalStatusOfReservedVersionsIsNotRead) {
  for (const unsigned v : {3u, 5u, 7u}) {
    const auto m = decode_vec(df17_with({{1, 5, 31}, {6, 8, 1}, {20, 20, 1}, {41, 43, v}, {44, 44, 1}, {45, 48, 9},
                                         {51, 52, 3}, {54, 54, 1}, {55, 55, 1}}));
    ASSERT_TRUE(m.has(kFieldOpStatus));
    EXPECT_EQ(m.op_status.version, v);
    EXPECT_FALSE(m.op_status.nic_supplement_a || m.op_status.nic_supplement_c || m.op_status.sil_per_sample);
    EXPECT_EQ(m.op_status.nac_p, 0);
    EXPECT_EQ(m.op_status.sil, 0);
  }
}

// DO-260B Table 2-14 (airborne) and 2-15 (surface), with readsb's readings
// for versions 0 and 1 (track.c compute_nic, v3.16.21).
TEST(Decoder, PositionNicFollowsTheTables) {
  // Type codes whose NIC needs no supplement, in every version.
  const std::pair<unsigned, unsigned> fixed[] = {{5, 11}, {6, 10}, {9, 11}, {10, 10}, {12, 7}, {13, 6},
                                                 {14, 5}, {15, 4}, {17, 1}, {18, 0}, {20, 11}, {21, 10}, {22, 0}};
  for (const auto& [tc, nic] : fixed) {
    for (unsigned v = 0; v <= 2; ++v) {
      for (unsigned bits = 0; bits < 8; ++bits) {
        ASSERT_EQ(position_nic(tc, v, bits & 1, bits & 2, bits & 4), nic) << tc << " v" << v << " " << bits;
      }
    }
  }
  // TC 11 (airborne, Rc 0.1 / 0.2 NM): version 2 needs A and B, version 1 A.
  EXPECT_EQ(position_nic(11, 2, true, true, false), 9);
  EXPECT_EQ(position_nic(11, 2, true, false, false), 8);
  EXPECT_EQ(position_nic(11, 2, false, true, false), 8);
  EXPECT_EQ(position_nic(11, 1, true, false, false), 9);
  EXPECT_EQ(position_nic(11, 1, false, true, false), 8) << "bit 8 is the single antenna flag in version 1";
  EXPECT_EQ(position_nic(11, 0, true, true, false), 8);
  // TC 16 (Rc 4 / 8 NM): A and B, in every version (readsb).
  EXPECT_EQ(position_nic(16, 2, true, true, false), 3);
  EXPECT_EQ(position_nic(16, 2, true, false, false), 2);
  EXPECT_EQ(position_nic(16, 0, true, true, false), 3);
  EXPECT_EQ(position_nic(16, 1, false, true, false), 2);
  // TC 7 (surface, Rc 0.1 / 0.2 NM): version 2 A without C, version 1 A.
  EXPECT_EQ(position_nic(7, 2, true, false, false), 9);
  EXPECT_EQ(position_nic(7, 2, true, false, true), 8);
  EXPECT_EQ(position_nic(7, 2, false, false, false), 8);
  EXPECT_EQ(position_nic(7, 1, true, false, true), 9);
  EXPECT_EQ(position_nic(7, 0, true, false, false), 8);
  // TC 8 (surface, Rc 0.6 NM or more): version 2 only.
  EXPECT_EQ(position_nic(8, 2, true, false, true), 7);
  EXPECT_EQ(position_nic(8, 2, true, false, false), 6);
  EXPECT_EQ(position_nic(8, 2, false, false, true), 6);
  EXPECT_EQ(position_nic(8, 2, false, false, false), 0);
  EXPECT_EQ(position_nic(8, 1, true, false, true), 0);
  // Reserved versions read as version 0 (as in readsb); non-position codes are 0.
  EXPECT_EQ(position_nic(11, 3, true, true, false), 8);
  EXPECT_EQ(position_nic(7, 5, true, false, false), 8);
  for (const unsigned tc : {0u, 1u, 4u, 19u, 23u, 28u, 29u, 31u, 32u}) {
    EXPECT_EQ(position_nic(tc, 2, true, true, true), 0) << tc;
  }
}

DecodedMsg decode_ap(const char* s) {
  const auto f = hex(s);
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kAddressParity) << s;
  EXPECT_TRUE(m.has(kFieldCommB)) << s;
  return m;
}

// Published Comm-B replies (pyModeS test suite); the addresses recovered
// from the parity are those of the aircraft that sent them.
TEST(Decoder, CommBIdentification) {
  auto m = decode_ap("A000083E202CC371C31DE0AA1CCF");
  EXPECT_EQ(m.icao, 0x484163u);
  EXPECT_TRUE(m.has(kFieldAltitude));  // the DF20 altitude is still decoded
  EXPECT_EQ(m.comm_b.candidates, CommB::kBds20);
  EXPECT_EQ(m.comm_b.bds, 0x20);
  EXPECT_EQ(m.comm_b.valid, CommB::kCallsign);
  EXPECT_EQ(std::string(m.comm_b.callsign, 8), "KLM1017 ");
  EXPECT_FALSE(m.has(kFieldCallsign));  // Comm-B values stay out of the ADS-B fields
  m = decode_ap("A0001993202422F2E37CE038738E");
  EXPECT_EQ(m.icao, 0x4CA532u);
  EXPECT_EQ(std::string(m.comm_b.callsign, 8), "IBK2873 ");
}

TEST(Decoder, CommBSelectedVerticalIntention) {
  const auto m = decode_ap("A000029C85E42F313000007047D3");
  EXPECT_EQ(m.comm_b.bds, 0x40);
  EXPECT_EQ(m.comm_b.valid, CommB::kMcpAltitude | CommB::kFmsAltitude | CommB::kBaroSetting);
  EXPECT_EQ(m.comm_b.mcp_altitude_ft, 3008);
  EXPECT_EQ(m.comm_b.fms_altitude_ft, 3008);
  EXPECT_NEAR(m.comm_b.baro_setting_hpa, 1020.0, 1e-3);
}

TEST(Decoder, CommBTrackAndTurn) {
  const auto m = decode_ap("A000139381951536E024D4CCF6B5");
  EXPECT_EQ(m.comm_b.bds, 0x50);
  EXPECT_EQ(m.comm_b.valid, CommB::kRoll | CommB::kTrueTrack | CommB::kGroundSpeed | CommB::kTrackRate | CommB::kTrueAirspeed);
  EXPECT_NEAR(m.comm_b.roll_deg, 2.109375, 1e-5);
  EXPECT_NEAR(m.comm_b.true_track_deg, 114.2578125, 1e-4);
  EXPECT_EQ(m.comm_b.ground_speed_kt, 438.0f);
  EXPECT_NEAR(m.comm_b.track_rate_deg_s, 0.125, 1e-6);
  EXPECT_EQ(m.comm_b.true_airspeed_kt, 424.0f);
}

TEST(Decoder, CommBHeadingAndSpeed) {
  const auto m = decode_ap("A00004128F39F91A7E27C46ADC21");
  EXPECT_EQ(m.comm_b.bds, 0x60);
  EXPECT_NEAR(m.comm_b.magnetic_heading_deg, 42.71484375, 1e-4);
  EXPECT_EQ(m.comm_b.indicated_airspeed_kt, 252.0f);
  EXPECT_NEAR(m.comm_b.mach, 0.42, 1e-5);
  EXPECT_EQ(m.comm_b.baro_vertical_rate_fpm, -1920);
  EXPECT_EQ(m.comm_b.inertial_vertical_rate_fpm, -1920);
}

TEST(Decoder, CommBCapabilityReports) {
  auto m = decode_ap("A800178D10010080F50000D5893C");  // DF21: the squawk is still decoded
  EXPECT_TRUE(m.has(kFieldSquawk));
  EXPECT_EQ(m.comm_b.bds, 0x10);
  EXPECT_EQ(m.comm_b.valid, 0u);
  m = decode_ap("A0000638FA81C10000000081A92F");
  EXPECT_EQ(m.comm_b.bds, 0x17);
}

// MB fields built bit by bit (MB bit n = bit n of the 56-bit value, MSB first).
CommB comm_b_of(std::initializer_list<std::tuple<unsigned, unsigned, std::uint64_t>> bits) {
  std::vector<std::uint8_t> mb(7, 0);
  for (const auto& [a, z, v] : bits) set_bits(mb, a, z, v);
  CommB c;
  decode_comm_b(mb.data(), c);
  return c;
}

TEST(Decoder, CommBFormatRules) {
  EXPECT_EQ(comm_b_of({}).candidates, 0u);  // empty MB: nothing
  // 5,0 alone: roll, track, gs and tas consistent. (6,0 reads bits 14-23 as
  // IAS with bit 13 its status: clear here, so 6,0 is ruled out.)
  auto c = comm_b_of({{1, 1, 1}, {3, 11, 20}, {12, 12, 1}, {14, 23, 100}, {24, 24, 1}, {25, 34, 200}, {46, 46, 1},
                      {47, 56, 190}});
  EXPECT_EQ(c.candidates, CommB::kBds50);
  // A value without its status bit rules a register out.
  c = comm_b_of({{1, 1, 0}, {3, 11, 20}, {24, 24, 1}, {25, 34, 200}, {46, 46, 1}, {47, 56, 190}});
  EXPECT_FALSE(c.candidates & CommB::kBds50);
  // Roll beyond 50 degrees, or ground speed and airspeed 200 kt apart: not 5,0.
  c = comm_b_of({{1, 1, 1}, {3, 11, 300}});
  EXPECT_FALSE(c.candidates & CommB::kBds50);
  c = comm_b_of({{24, 24, 1}, {25, 34, 300}, {46, 46, 1}, {47, 56, 150}});
  EXPECT_FALSE(c.candidates & CommB::kBds50);
  // Negative values (two's complement after the sign bit).
  c = comm_b_of({{1, 1, 1}, {2, 2, 1}, {3, 11, 512 - 64}, {12, 12, 1}, {13, 13, 1}, {14, 23, 1024 - 512}});
  ASSERT_EQ(c.bds, 0x50);
  EXPECT_NEAR(c.roll_deg, -64 * 45.0 / 256.0, 1e-5);
  EXPECT_NEAR(c.true_track_deg, 360.0 - 90.0, 1e-4);
  // 6,0 with a negative heading (west of north) and climb rates: heading
  // wraps to [0, 360). The IAS (13 status, 14-23) is 5,0's track value with
  // bit 12, the heading's last bit (0 here), as its status: so not 5,0.
  c = comm_b_of({{1, 1, 1}, {2, 2, 1}, {3, 12, 1024 - 64}, {13, 13, 1}, {14, 23, 250}, {35, 35, 1}, {37, 45, 20},
                 {46, 46, 1}, {48, 56, 22}});
  ASSERT_EQ(c.candidates, CommB::kBds60);
  EXPECT_EQ(c.indicated_airspeed_kt, 250.0f);
  EXPECT_NEAR(c.magnetic_heading_deg, 360.0 - 64 * 90.0 / 512.0, 1e-4);
  EXPECT_EQ(c.baro_vertical_rate_fpm, 640);
  EXPECT_EQ(c.inertial_vertical_rate_fpm, 704);
  // 6,0: Mach above 1 or IAS above 500 kt rule it out.
  c = comm_b_of({{24, 24, 1}, {25, 34, 300}});
  EXPECT_FALSE(c.candidates & CommB::kBds60);
  c = comm_b_of({{13, 13, 1}, {14, 23, 600}});
  EXPECT_FALSE(c.candidates & CommB::kBds60);
  // Baro and inertial rates 1,500 ft/min apart (a transient) are still 6,0;
  // a rate beyond 6,000 ft/min is not.
  c = comm_b_of({{1, 1, 1}, {3, 12, 300}, {13, 13, 1}, {14, 23, 250}, {35, 35, 1}, {37, 45, 10}, {46, 46, 1}, {48, 56, 57}});
  EXPECT_EQ(c.candidates, CommB::kBds60);
  c = comm_b_of({{35, 35, 1}, {36, 36, 1}, {37, 45, 512 - 190}});
  EXPECT_FALSE(c.candidates & CommB::kBds60);
  // 4,0: a reserved bit set rules it out; a baro setting outside 850-1100 too.
  c = comm_b_of({{1, 1, 1}, {2, 13, 200}, {45, 45, 1}});
  EXPECT_FALSE(c.candidates & CommB::kBds40);
  c = comm_b_of({{27, 27, 1}, {28, 39, 3001}});  // 1100.1 hPa
  EXPECT_FALSE(c.candidates & CommB::kBds40);
  // A field both 5,0 and 6,0 accept: both are candidates and nothing is decoded.
  c = comm_b_of({{1, 1, 1}, {3, 11, 10}});
  EXPECT_TRUE(c.candidates & CommB::kBds50);
  EXPECT_TRUE(c.candidates & CommB::kBds60);
  EXPECT_EQ(c.bds, 0);
  EXPECT_EQ(c.valid, 0u);
  // 2,0 needs valid characters; 3,0 rejects threat type 3.
  c = comm_b_of({{1, 8, 0x20}, {9, 14, 0}});
  EXPECT_FALSE(c.candidates & CommB::kBds20);
  c = comm_b_of({{1, 8, 0x30}, {29, 30, 3}});
  EXPECT_FALSE(c.candidates & CommB::kBds30);
  c = comm_b_of({{1, 8, 0x30}, {29, 30, 1}});
  EXPECT_TRUE(c.candidates & CommB::kBds30);
}

TEST(Decoder, SingleBitRepairIsOffByDefault) {
  auto f = hex("8D4840D6202CC371C32CE0576098");
  f[6] ^= 0x10;
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m), DecodeStatus::kBadCrc);
  EXPECT_EQ(m.corrected_bit, -1);
}

TEST(Decoder, SingleBitRepairFixesEveryBitOutsideTheFormatField) {
  const auto clean = hex("8D4840D6202CC371C32CE0576098");
  DecodedMsg ref;
  ASSERT_EQ(decode(clean.data(), clean.size(), ref), DecodeStatus::kOk);
  const DecodeOptions fix{true};
  for (int i = 0; i < 112; ++i) {
    auto f = clean;
    f[static_cast<std::size_t>(i / 8)] ^= static_cast<std::uint8_t>(0x80u >> (i % 8));
    DecodedMsg m;
    const DecodeStatus st = decode(f.data(), f.size(), m, fix);
    if (i < 5) {
      // A flipped format bit makes it another format: never "repaired" into DF17.
      EXPECT_FALSE(st == DecodeStatus::kOk && m.df == 17 && m.corrected_bit >= 0) << i;
      continue;
    }
    ASSERT_EQ(st, DecodeStatus::kOk) << i;
    EXPECT_EQ(m.corrected_bit, i);
    EXPECT_EQ(m.icao, ref.icao) << i;
    EXPECT_EQ(callsign(m), callsign(ref)) << i;
    EXPECT_NE(m.syndrome, 0u);  // the received frame's
  }
}

// A DF19 (military) reply with format bit 3 wrong reads as a DF17 whose
// syndrome is exactly that of a format-bit error: it is not "repaired" into
// DF19 (format bits are never repaired).
TEST(Decoder, SingleBitRepairNeverTouchesTheFormatField) {
  std::vector<std::uint8_t> f(14, 0);
  set_bits(f, 1, 5, 19);
  set_bits(f, 9, 32, 0xABCDEF);
  fix_parity(f);
  f[0] = static_cast<std::uint8_t>(f[0] ^ 0x10);  // bit 4 (1-indexed): DF19 -> DF17
  ASSERT_EQ(f[0] >> 3, 17);
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m, DecodeOptions{true}), DecodeStatus::kBadCrc);
  EXPECT_EQ(m.corrected_bit, -1);
}

TEST(Decoder, SingleBitRepairNeverTakesTwoWrongBits) {
  // The Mode S CRC's distance is 6 at 112 bits: a two-bit error never has
  // a single-bit syndrome, so it is never "repaired" into something else.
  const auto clean = hex("8D40621D58C382D690C8AC2863A7");
  const DecodeOptions fix{true};
  int rejected = 0;
  for (int i = 5; i < 112; ++i) {
    for (int j = i + 1; j < 112; ++j) {
      auto f = clean;
      f[static_cast<std::size_t>(i / 8)] ^= static_cast<std::uint8_t>(0x80u >> (i % 8));
      f[static_cast<std::size_t>(j / 8)] ^= static_cast<std::uint8_t>(0x80u >> (j % 8));
      DecodedMsg m;
      rejected += decode(f.data(), f.size(), m, fix) == DecodeStatus::kBadCrc;
    }
  }
  EXPECT_EQ(rejected, 107 * 106 / 2);
}

TEST(Decoder, SingleBitRepairLeavesAddressParityFormatsAlone) {
  auto f = hex("A000083E202CC371C31DE0AA1CCF");
  f[8] ^= 0x01;
  DecodedMsg m;
  EXPECT_EQ(decode(f.data(), f.size(), m, DecodeOptions{true}), DecodeStatus::kAddressParity);
  EXPECT_EQ(m.corrected_bit, -1);
  EXPECT_NE(m.icao, 0x484163u);  // the syndrome, as always for these formats
}

}  // namespace
