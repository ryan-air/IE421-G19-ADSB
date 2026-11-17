#include "adsb/modes/encoder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <cstring>
#include <string>

#include "adsb/common/rng.hpp"
#include "adsb/modes/altitude.hpp"
#include "adsb/modes/decoder.hpp"
#include "test_util.hpp"

namespace {

using namespace adsb::modes;

DecodedMsg must_decode(const Frame& f, DecodeStatus want = DecodeStatus::kOk) {
  DecodedMsg m;
  EXPECT_EQ(decode(f.bytes, f.len, m), want);
  return m;
}

TEST(Encoder, ReproducesPublishedIdentificationFrame) {
  // The encoder must produce the exact bytes of the Riddle's KLM1023 example.
  const Frame f = encode_identification(0x4840D6, "KLM1023 ", 0xA0, 5);
  const auto want = adsb::test::hex("8D4840D6202CC371C32CE0576098");
  ASSERT_EQ(f.len, 14);
  EXPECT_EQ(std::memcmp(f.bytes, want.data(), 14), 0);
}

TEST(Encoder, ReproducesPublishedPositionFrame) {
  const Frame f = encode_airborne_position(0x40621D, 38000, CprFrame{93000, 51372, false}, 11, 5);
  const auto want = adsb::test::hex("8D40621D58C382D690C8AC2863A7");
  EXPECT_EQ(std::memcmp(f.bytes, want.data(), 14), 0);
}

TEST(Encoder, AltitudeCodeRoundTrip) {
  for (std::int32_t alt = -1000; alt <= 50175; alt += 25) {
    const auto ac12 = encode_ac12(alt);
    ASSERT_TRUE(ac12) << alt;
    ASSERT_EQ(decode_ac12(*ac12).value_or(-99999), alt);
    ASSERT_EQ(decode_ac13(*encode_ac13(alt)).value_or(-99999), alt);
  }
  EXPECT_EQ(decode_ac12(*encode_ac12(1012)).value_or(0), 1000) << "rounds to nearest 25 ft";
  EXPECT_EQ(decode_ac12(*encode_ac12(1013)).value_or(0), 1025);
  EXPECT_FALSE(encode_ac12(-1013));
  EXPECT_FALSE(encode_ac12(50200));
}

TEST(Encoder, SquawkFieldRoundTrip) {
  for (unsigned a = 0; a < 8; ++a)
    for (unsigned b = 0; b < 8; ++b)
      for (unsigned c = 0; c < 8; ++c)
        for (unsigned d = 0; d < 8; ++d) {
          const auto sq = static_cast<std::uint16_t>((a << 12) | (b << 8) | (c << 4) | d);
          ASSERT_EQ(id13_to_mode_a(mode_a_to_id13(sq)), sq);
        }
}

TEST(Encoder, VelocityRoundTrip) {
  adsb::Rng rng(3);
  for (int i = 0; i < 20'000; ++i) {
    const double gs = rng.uniform(5, 900);
    const double trk = rng.uniform(0, 360);
    const auto vr = static_cast<std::int32_t>(rng.uniform(-6000, 6000));
    const double e = gs * std::sin(trk * std::numbers::pi / 180), n = gs * std::cos(trk * std::numbers::pi / 180);
    const DecodedMsg m = must_decode(encode_velocity(0xABCDEF, e, n, vr));
    ASSERT_TRUE(m.has(kFieldGroundSpeed));
    // Each component is quantized to 1 kt.
    ASSERT_NEAR(m.ground_speed_kt, gs, 1.0);
    if (gs > 30) {
      ASSERT_NEAR(std::remainder(static_cast<double>(m.track_deg) - trk, 360.0), 0.0, 2.5);
    }
    ASSERT_NEAR(m.vertical_rate_fpm, vr, 32 + 1);
    ASSERT_FALSE(m.vertical_rate_gnss);
  }
}

TEST(Encoder, PositionRoundTripThroughDecoderAndCpr) {
  adsb::Rng rng(4);
  for (int i = 0; i < 20'000; ++i) {
    const double lat = rng.uniform(-85, 85), lon = rng.uniform(-180, 180);
    const auto alt = static_cast<std::int32_t>(25 * std::lround(rng.uniform(0, 45000) / 25));
    const DecodedMsg e = must_decode(encode_airborne_position(0x123456, alt, cpr_encode(lat, lon, false, false)));
    const DecodedMsg o = must_decode(encode_airborne_position(0x123456, alt, cpr_encode(lat, lon, true, false)));
    ASSERT_EQ(e.altitude_ft, alt);
    ASSERT_FALSE(e.cpr.odd);
    ASSERT_TRUE(o.cpr.odd);
    const auto p = cpr_global_airborne(e.cpr, o.cpr, true);
    if (!p) continue;  // NL boundary straddle
    // Compare on the ground: CPR longitude resolution in *degrees* grows
    // toward the poles (fewer, wider zones), but stays ~5 m in distance.
    const double dy = (p->lat - lat) * 111'320.0;
    const double dx = std::remainder(p->lon - lon, 360.0) * 111'320.0 * std::cos(lat * std::numbers::pi / 180);
    ASSERT_LT(std::hypot(dx, dy), 6.0) << lat << "," << lon;
  }
}

TEST(Encoder, SurfaceMovementRoundTrip) {
  for (unsigned code = 1; code <= 124; ++code) {
    const double kt = surface_movement_to_knots(code);
    ASSERT_EQ(knots_to_surface_movement(kt), code) << kt;
  }
  EXPECT_EQ(knots_to_surface_movement(-1), 0u);
  EXPECT_EQ(knots_to_surface_movement(500), 124u);
  // Arbitrary speeds land on the nearest representable value.
  for (double kt = 0; kt < 200; kt += 0.37) {
    const double back = surface_movement_to_knots(knots_to_surface_movement(kt));
    ASSERT_LE(std::fabs(back - kt), kt < 175 ? 2.6 : 30) << kt;
  }
}

TEST(Encoder, SurfacePositionAndShortFormats) {
  const DecodedMsg s = must_decode(encode_surface_position(0x484175, 17.0, 92.8, cpr_encode(52.32, 4.73, true, true)));
  EXPECT_EQ(s.tc, 7);
  EXPECT_TRUE(s.cpr_surface);
  EXPECT_DOUBLE_EQ(s.ground_speed_kt, 17.0);
  EXPECT_NEAR(s.track_deg, 92.8, 360.0 / 128);
  EXPECT_TRUE(s.on_ground);

  const DecodedMsg df11 = must_decode(encode_df11(0x4840D6, 5));
  EXPECT_EQ(df11.df, 11);
  EXPECT_EQ(df11.icao, 0x4840D6u);

  const DecodedMsg df4 = must_decode(encode_df4(0x4840D6, 37000, 1), DecodeStatus::kAddressParity);
  EXPECT_EQ(df4.icao, 0x4840D6u);
  EXPECT_EQ(df4.altitude_ft, 37000);
  EXPECT_TRUE(df4.on_ground);

  const DecodedMsg df5 = must_decode(encode_df5(0x4840D6, 0x7700), DecodeStatus::kAddressParity);
  EXPECT_EQ(df5.icao, 0x4840D6u);
  EXPECT_EQ(df5.squawk, 0x7700);
}

TEST(Encoder, CallsignNormalization) {
  const DecodedMsg m = must_decode(encode_identification(0x1, "ual12_x!", 0xA3));
  EXPECT_EQ(std::string(m.callsign, 8), "UAL12 X ");
  EXPECT_EQ(m.category, 0xA3);
}

}  // namespace
