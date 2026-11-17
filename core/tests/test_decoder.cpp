#include "adsb/modes/decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
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

}  // namespace
