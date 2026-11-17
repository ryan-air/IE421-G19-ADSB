#include "adsb/modes/cpr.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace {

using namespace adsb::modes;

// Great-circle distance in nautical miles (haversine).
double distance_nm(const LatLon& a, const LatLon& b) {
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  const double dlat = (b.lat - a.lat) * kDeg;
  const double dlon = (b.lon - a.lon) * kDeg;
  const double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(a.lat * kDeg) * std::cos(b.lat * kDeg) * std::sin(dlon / 2) *
                       std::sin(dlon / 2);
  return 2.0 * 3440.065 * std::asin(std::sqrt(h));
}

TEST(CprNl, TableMatchesClosedForm) {
  // Sample densely, skipping points within 1e-6 deg of a zone transition where
  // the 8-decimal table and floating-point evaluation may legitimately differ.
  int checked = 0;
  for (double lat = -89.99; lat <= 89.99; lat += 0.0007) {
    const int ref = cpr_nl_reference(lat);
    if (cpr_nl_reference(lat - 1e-6) != ref || cpr_nl_reference(lat + 1e-6) != ref) continue;
    ASSERT_EQ(cpr_nl(lat), ref) << "lat=" << lat;
    ++checked;
  }
  EXPECT_GT(checked, 200'000);
}

TEST(CprNl, SpecialValues) {
  EXPECT_EQ(cpr_nl(0.0), 59);
  EXPECT_EQ(cpr_nl(-0.0), 59);
  EXPECT_EQ(cpr_nl(87.0), 2);
  EXPECT_EQ(cpr_nl(-87.0), 2);
  EXPECT_EQ(cpr_nl(87.0001), 1);
  EXPECT_EQ(cpr_nl(90.0), 1);
  EXPECT_EQ(cpr_nl(52.25720214843750), 36) << "worked example in The 1090 MHz Riddle";
}

// "The 1090 MHz Riddle", airborne position example (ICAO 40621D):
//   even 8D40621D58C382D690C8AC2863A7  lat_cpr 93000  lon_cpr 51372
//   odd  8D40621D58C386435CC412692AD6  lat_cpr 74158  lon_cpr 50194
// With the even frame newest: lat 52.25720, lon 3.91937.
const CprFrame kEven40621D{93000, 51372, false};
const CprFrame kOdd40621D{74158, 50194, true};

TEST(CprAirborne, GlobalRiddleExample) {
  const auto p = cpr_global_airborne(kEven40621D, kOdd40621D, /*odd_is_newer=*/false);
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(p->lat, 52.25720, 1e-5);
  EXPECT_NEAR(p->lon, 3.91937, 1e-5);
}

TEST(CprAirborne, GlobalRiddleExampleOddNewer) {
  // Odd-frame solution of the same pair. Expected values come from evaluating
  // the book's global-decoding formulas directly (independently of this
  // implementation): lat = dlat_odd * (j mod 59 + lat_cpr_odd), etc.
  const auto p = cpr_global_airborne(kEven40621D, kOdd40621D, /*odd_is_newer=*/true);
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(p->lat, 52.26578017, 1e-7);
  EXPECT_NEAR(p->lon, 3.93891253, 1e-7);
}

TEST(CprAirborne, LocalRiddleExample) {
  // Same even frame, decoded against a reference position nearby.
  const LatLon p = cpr_local_airborne(kEven40621D, {52.258, 3.918});
  EXPECT_NEAR(p.lat, 52.25720, 1e-5);
  EXPECT_NEAR(p.lon, 3.91937, 1e-5);
}

TEST(CprAirborne, RoundTripEverywhere) {
  // Encode random positions as even+odd pairs and decode globally and locally.
  std::mt19937_64 rng(2024);
  std::uniform_real_distribution<double> lat_d(-87.0, 87.0);
  std::uniform_real_distribution<double> lon_d(-180.0, 180.0);
  int global_ok = 0;
  for (int i = 0; i < 100'000; ++i) {
    const LatLon truth{lat_d(rng), lon_d(rng)};
    const CprFrame e = cpr_encode(truth.lat, truth.lon, false, false);
    const CprFrame o = cpr_encode(truth.lat, truth.lon, true, false);
    ASSERT_LT(e.lat, 1u << 17);
    ASSERT_LT(o.lon, 1u << 17);

    for (const bool odd_newer : {false, true}) {
      const auto g = cpr_global_airborne(e, o, odd_newer);
      // Pairs straddling an NL boundary are correctly rejected; everything
      // else must decode to within the ~5 m airborne resolution.
      if (!g) continue;
      ++global_ok;
      ASSERT_LT(distance_nm(*g, truth), 0.005)
          << truth.lat << "," << truth.lon << " -> " << g->lat << "," << g->lon;
    }

    // Local decode against a reference ~60-170 NM away (must be < 180 NM).
    const LatLon ref{truth.lat + 1.0, truth.lon - 1.0};
    if (std::fabs(ref.lat) < 87.0 && distance_nm(ref, truth) < 170.0) {
      const LatLon l = cpr_local_airborne(e, ref);
      ASSERT_LT(distance_nm(l, truth), 0.005) << truth.lat << "," << truth.lon;
    }
  }
  EXPECT_GT(global_ok, 190'000) << "almost all pairs should be in the same NL band";
}

TEST(CprAirborne, RejectsPairAcrossNlBoundary) {
  // Even and odd frames from positions on either side of the NL 36/35
  // transition (53.09516 deg) must not be combined.
  const CprFrame e = cpr_encode(53.08, 10.0, false, false);
  const CprFrame o = cpr_encode(53.11, 10.0, true, false);
  EXPECT_FALSE(cpr_global_airborne(e, o, true).has_value());
}

TEST(CprAirborne, SouthernAndWesternHemispheres) {
  for (const LatLon truth : {LatLon{-33.9461, 151.1772},    // Sydney
                             LatLon{-34.8222, -58.5358},    // Buenos Aires
                             LatLon{40.1201, -88.2786},     // Champaign
                             LatLon{0.0001, -179.9999}}) {  // near antimeridian
    const auto g = cpr_global_airborne(cpr_encode(truth.lat, truth.lon, false, false),
                                       cpr_encode(truth.lat, truth.lon, true, false), false);
    ASSERT_TRUE(g.has_value());
    EXPECT_LT(distance_nm(*g, truth), 0.005) << truth.lat << "," << truth.lon;
    EXPECT_GE(g->lon, -180.0);
    EXPECT_LT(g->lon, 180.0);
  }
}

TEST(CprSurface, RoundTripNearReceiver) {
  std::mt19937_64 rng(77);
  std::uniform_real_distribution<double> lat_d(-80.0, 80.0);
  std::uniform_real_distribution<double> lon_d(-180.0, 180.0);
  std::uniform_real_distribution<double> off_d(-0.3, 0.3);  // ~18 NM
  for (int i = 0; i < 50'000; ++i) {
    const LatLon receiver{lat_d(rng), lon_d(rng)};
    const LatLon truth{receiver.lat + off_d(rng), receiver.lon + off_d(rng)};
    if (distance_nm(truth, receiver) > 40.0) continue;
    const CprFrame e = cpr_encode(truth.lat, truth.lon, false, true);
    const CprFrame o = cpr_encode(truth.lat, truth.lon, true, true);

    const auto g = cpr_global_surface(e, o, false, receiver);
    if (g) {
      ASSERT_LT(distance_nm(*g, truth), 0.002)
          << "truth " << truth.lat << "," << truth.lon << " got " << g->lat << "," << g->lon;
    }
    const LatLon l = cpr_local_surface(o, receiver);
    ASSERT_LT(distance_nm(l, truth), 0.002);
  }
}

}  // namespace
