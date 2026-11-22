// Tests for the synthetic IQ scenario generator (sim/iq.hpp) and the
// portable math it relies on.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "adsb/common/hash.hpp"
#include "adsb/common/portable_math.hpp"
#include "adsb/modes/encoder.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb::sim;

constexpr double kC = 299'792'458.0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

SynthConfig one_message_synth() {
  SynthConfig s;
  s.duration_s = 0.002;
  return s;
}

Transmission transmission(const SynthConfig& s, const adsb::modes::Frame& f, double north_deg, double alt_ft,
                          double t_s = 0.0002) {
  Transmission tr;
  tr.tx_ns = s.start_rx_ns + static_cast<std::uint64_t>(t_s * 1e9);
  tr.icao = 0xABCDEF;
  tr.len = f.len;
  std::memcpy(tr.bytes, f.bytes, f.len);
  tr.lat = s.receiver_lat + north_deg;
  tr.lon = s.receiver_lon;
  tr.alt_ft = alt_ft;
  tr.ground_speed_kt = 0;
  return tr;
}

SynthOutput traffic_of(std::vector<Transmission> txs) {
  SynthOutput out;
  for (const auto& t : txs) {
    bool known = false;
    for (const auto& a : out.aircraft) known = known || a.icao == t.icao;
    if (!known) {
      AircraftInfo a;
      a.icao = t.icao;
      out.aircraft.push_back(a);
    }
  }
  out.transmissions = std::move(txs);
  return out;
}

// A clean config: no noise, no multipath, omni antenna, no transmitter
// randomness unless a test sets it.
IqConfig clean_config(double rate = 2.4e6) {
  IqConfig c;
  c.sample_rate_hz = rate;
  c.noise = false;
  c.ground_reflection = 0.0;
  c.antenna = AntennaPattern::kOmni;
  c.tx_offset_sd_hz = 0.0;
  c.tx_drift_sd_hz_per_s = 0.0;
  c.tx_jitter_sd_hz = 0.0;
  c.full_scale_dbm = -40.0;  // nothing clips
  return c;
}

std::vector<float> render_all(const IqPlan& plan, const IqConfig& cfg) {
  std::vector<float> out(2 * plan.total_samples);
  std::vector<float> chunk(2 * kIqChunkSamples);
  for (std::uint64_t c = 0;; ++c) {
    const std::size_t n = render_iq_chunk(plan, cfg, c, chunk.data());
    if (n == 0) break;
    std::memcpy(out.data() + 2 * c * kIqChunkSamples, chunk.data(), 2 * n * sizeof(float));
  }
  return out;
}

double mag(const std::vector<float>& iq, double pos) {
  const auto i = static_cast<std::size_t>(std::floor(pos));
  const double f = pos - static_cast<double>(i);
  auto m = [&](std::size_t k) { return std::hypot(static_cast<double>(iq[2 * k]), static_cast<double>(iq[2 * k + 1])); };
  return m(i) * (1 - f) + m(i + 1) * f;
}

// Reference slicer: compares the energy of the two half-bits of every bit at
// the annotated position. Not a demodulator (it is told where the message
// is); it checks that the waveform carries the right bits.
std::vector<std::uint8_t> slice(const std::vector<float>& iq, double sample_pos, double fs, int len) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(len), 0);
  for (int b = 0; b < len * 8; ++b) {
    double e[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
      for (int k = 0; k < 5; ++k) {
        const double t_us = 8.0 + b + 0.5 * h + 0.1 + 0.3 * k / 4.0;
        e[h] += mag(iq, sample_pos + t_us * 1e-6 * fs);
      }
    }
    if (e[0] > e[1]) out[static_cast<std::size_t>(b / 8)] |= static_cast<std::uint8_t>(0x80 >> (b % 8));
  }
  return out;
}

std::vector<float> dequantize(const std::vector<std::uint8_t>& q, IqFormat f, std::size_t n) {
  std::vector<float> out(2 * n);
  for (std::size_t i = 0; i < 2 * n; ++i) {
    switch (f) {
      case IqFormat::kCu8: out[i] = static_cast<float>((q[i] - 127.5) / 127.5); break;
      case IqFormat::kCs8: out[i] = static_cast<float>(static_cast<std::int8_t>(q[i]) / 127.0); break;
      case IqFormat::kCs16: {
        const auto v = static_cast<std::int16_t>(q[2 * i] | (q[2 * i + 1] << 8));
        out[i] = static_cast<float>(v / 32767.0);
        break;
      }
      case IqFormat::kCf32: {
        std::uint32_t u = 0;
        for (int b = 0; b < 4; ++b) u |= std::uint32_t{q[4 * i + static_cast<std::size_t>(b)]} << (8 * b);
        float v;
        std::memcpy(&v, &u, 4);
        out[i] = v;
        break;
      }
    }
  }
  return out;
}

// Frequency from the phase advance between consecutive samples inside the
// pulses. Samples whose averaging window overlaps a pulse edge have a shifted
// phase centroid, so `inside` (fraction of the peak magnitude) selects how
// strictly they are excluded.
double estimate_hz(const std::vector<float>& iq, const IqMessage& m, double fs, double inside = 0.9) {
  double sr = 0, si = 0;
  const auto a = static_cast<std::size_t>(m.first_sample);
  const auto b = static_cast<std::size_t>(m.last_sample);
  double peak = 0;
  for (std::size_t i = a; i <= b; ++i) {
    peak = std::max(peak, std::hypot(static_cast<double>(iq[2 * i]), static_cast<double>(iq[2 * i + 1])));
  }
  for (std::size_t i = a; i < b; ++i) {
    const double i0 = static_cast<double>(iq[2 * i]), q0 = static_cast<double>(iq[2 * i + 1]);
    const double i1 = static_cast<double>(iq[2 * i + 2]), q1 = static_cast<double>(iq[2 * i + 3]);
    if (std::hypot(i0, q0) < inside * peak || std::hypot(i1, q1) < inside * peak) continue;
    // x1 * conj(x0)
    sr += i1 * i0 + q1 * q0;
    si += q1 * i0 - i1 * q0;
  }
  return std::atan2(si, sr) * fs / (2 * adsb::pm::kPi);
}

adsb::modes::Frame df17() {
  const auto cpr = adsb::modes::cpr_encode(40.5, -88.3, false, false);
  return adsb::modes::encode_airborne_position(0xABCDEF, 30000, cpr);
}

// ---------------------------------------------------------------------------
// Portable math
// ---------------------------------------------------------------------------

TEST(PortableMath, MatchesLibmClosely) {
  for (double x = -1000.0; x <= 1000.0; x += 0.37) {
    double s, c;
    adsb::pm::sincos(x, s, c);
    EXPECT_NEAR(s, std::sin(x), 2e-15) << x;
    EXPECT_NEAR(c, std::cos(x), 2e-15) << x;
  }
  for (double x : {1e5 + 0.1, 6e5, 9.9e5}) EXPECT_NEAR(adsb::pm::sin(x), std::sin(x), 1e-10) << x;
  for (double x = 1e-300; x < 1e300; x *= 7.3) EXPECT_NEAR(adsb::pm::log(x) / std::log(x), 1.0, 4e-16) << x;
  EXPECT_DOUBLE_EQ(adsb::pm::log(1.0), 0.0);
  for (double x = -700; x <= 700; x += 0.731) EXPECT_NEAR(adsb::pm::exp(x) / std::exp(x), 1.0, 4e-15) << x;
  for (double x = -50; x <= 50; x += 0.0137) EXPECT_NEAR(adsb::pm::atan(x), std::atan(x), 4e-16 * 4) << x;
  for (double y : {-3.0, -0.5, 0.0, 0.7, 2.0}) {
    for (double x : {-2.0, -0.1, 0.0, 0.3, 4.0}) EXPECT_NEAR(adsb::pm::atan2(y, x), std::atan2(y, x), 1e-15);
  }
  EXPECT_NEAR(adsb::pm::power_to_db(adsb::pm::db_to_power(-73.25)), -73.25, 1e-12);
}

// ---------------------------------------------------------------------------
// Link budget and geometry
// ---------------------------------------------------------------------------

TEST(IqLink, FreeSpaceLossAndPower) {
  IqConfig cfg = clean_config();
  Site site{40.0, -88.0, 200.0, 10.0};
  Emitter e;
  e.lat = 40.0 + 100'000.0 / 111'000.0;  // about 100 km north
  e.lon = -88.0;
  e.alt_m = 210.0;                      // level with the antenna (no bending check here)
  e.power_dbm = 54.0;
  cfg.diffraction_db_per_km = 0.0;
  const LinkResult r = compute_link(site, e, cfg);
  const double expected_fspl = 20.0 * std::log10(4.0 * M_PI * r.range_m * kModeSCarrierHz / kC);
  EXPECT_NEAR(r.path_loss_db, expected_fspl, 1e-9);
  EXPECT_NEAR(expected_fspl, 133.2, 0.2);  // textbook value at 100 km, 1090 MHz
  EXPECT_NEAR(r.power_dbm, 54.0 - expected_fspl - cfg.cable_loss_db, 1e-9);
  EXPECT_EQ(r.n_paths, 1);
  EXPECT_NEAR(r.paths[0].amplitude, std::sqrt(std::pow(10.0, (r.power_dbm - cfg.full_scale_dbm) / 10.0)), 1e-12);
}

TEST(IqLink, DipoleGainFallsOffOverhead) {
  IqConfig cfg = clean_config();
  cfg.antenna = AntennaPattern::kDipole;
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter horizon, overhead;
  horizon.lat = 40.5;
  horizon.lon = -88.0;
  horizon.alt_m = 10.0 + 55'000.0 * 55'000.0 / (2.0 * 4.0 / 3.0 * 6'371'000.0);  // ~ zero elevation
  overhead.lat = 40.0001;
  overhead.lon = -88.0;
  overhead.alt_m = 10'000.0;
  const LinkResult h = compute_link(site, horizon, cfg);
  const LinkResult o = compute_link(site, overhead, cfg);
  EXPECT_NEAR(h.elevation_deg, 0.0, 0.3);
  EXPECT_NEAR(h.antenna_gain_db, 2.15, 0.05);
  EXPECT_GT(o.elevation_deg, 85.0);
  EXPECT_LT(o.antenna_gain_db, -15.0);
}

TEST(IqLink, DopplerSignAndSize) {
  IqConfig cfg = clean_config();
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter e;
  e.lat = 41.0;  // 111 km north, flying south (towards the receiver) at 250 m/s
  e.lon = -88.0;
  e.alt_m = 3000.0;
  e.vel_north_mps = -250.0;
  const LinkResult r = compute_link(site, e, cfg);
  EXPECT_LT(r.range_rate_mps, -245.0);
  EXPECT_GT(r.range_rate_mps, -250.0);
  EXPECT_NEAR(r.doppler_hz, -kModeSCarrierHz * r.range_rate_mps / kC, 1e-9);
  EXPECT_NEAR(r.doppler_hz, 909.0, 10.0);  // approaching: higher frequency
  EXPECT_NEAR(r.baseband_hz, r.doppler_hz, 1e-6);

  e.vel_north_mps = 250.0;  // receding
  EXPECT_LT(compute_link(site, e, cfg).doppler_hz, -890.0);
  cfg.doppler = false;
  EXPECT_EQ(compute_link(site, e, cfg).doppler_hz, 0.0);
}

TEST(IqLink, ReceiverLoErrorShiftsTheBaseband) {
  IqConfig cfg = clean_config();
  cfg.rx_ppm = 2.0;
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter e;
  e.lat = 40.5;
  e.lon = -88.0;
  e.alt_m = 9000.0;
  e.carrier_offset_hz = 50'000.0;
  const LinkResult r = compute_link(site, e, cfg);
  EXPECT_NEAR(r.baseband_hz, 50'000.0 - 2.0 * 1090.0, 1e-3);
}

TEST(IqLink, RadioHorizonAddsDiffractionLoss) {
  IqConfig cfg = clean_config();
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter low;
  low.lat = 40.0 + 200'000.0 / 111'000.0;  // 200 km
  low.lon = -88.0;
  low.alt_m = 300.0;                       // 1,000 ft: horizon about 84 km
  const LinkResult r = compute_link(site, low, cfg);
  const double re = 4.0 / 3.0 * 6'371'000.0;
  EXPECT_NEAR(r.horizon_m, std::sqrt(2 * re * 300.0) + std::sqrt(2 * re * 10.0), 1.0);
  const double fspl = 20.0 * std::log10(4.0 * M_PI * r.range_m * kModeSCarrierHz / kC);
  const double beyond_km = (r.ground_distance_m - r.horizon_m) / 1000.0;
  EXPECT_GT(beyond_km, 100.0);
  EXPECT_NEAR(r.path_loss_db - fspl, beyond_km * cfg.diffraction_db_per_km, 1e-6);
  EXPECT_LT(r.elevation_deg, 0.0);  // below the geometric horizon

  // Ducting (large k) moves the horizon out.
  cfg.k_factor = 4.0;
  EXPECT_GT(compute_link(site, low, cfg).horizon_m, 2.0 * r.horizon_m * 0.8);
}

// Brute force: the reflection point on a circle of radius re that minimizes
// the path from the receiver (height hr) to the transmitter (height ht, at
// ground distance d), minus the direct path. Independent of the code's
// fixed-point iteration.
double brute_force_bounce(double hr, double ht, double d, double re, double* d1_out = nullptr) {
  const double theta = d / re;
  auto pt = [&](double ang, double h) { return std::pair<double, double>{(re + h) * std::sin(ang), (re + h) * std::cos(ang)}; };
  auto dist = [](std::pair<double, double> a, std::pair<double, double> b) {
    return std::hypot(a.first - b.first, a.second - b.second);
  };
  const auto r = pt(0, hr), t = pt(theta, ht);
  auto len = [&](double phi) { return dist(r, pt(phi, 0)) + dist(pt(phi, 0), t); };
  double lo = 0, hi = theta;
  for (int i = 0; i < 300; ++i) {
    const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
    if (len(m1) < len(m2)) hi = m2; else lo = m1;
  }
  if (d1_out) *d1_out = re * (lo + hi) / 2;
  return len((lo + hi) / 2) - dist(r, t);
}

TEST(IqLink, TwoRayGroundBounceOverTheEffectiveEarth) {
  IqConfig cfg = clean_config();
  cfg.ground_reflection = 0.5;
  cfg.rx_ppm = 3.0;
  const double re = 4.0 / 3.0 * 6'371'000.0;
  Site site{40.0, -88.0, 0.0, 10.0};
  for (double dist_km : {10.0, 100.0, 200.0}) {
    Emitter e;
    e.lat = 40.0 + dist_km * 1000.0 / 111'000.0;
    e.lon = -88.0;
    e.alt_m = 10'000.0;
    const LinkResult r = compute_link(site, e, cfg);
    ASSERT_EQ(r.n_paths, 2) << dist_km;
    const double want = brute_force_bounce(10.0, 10'000.0, r.ground_distance_m, re);
    EXPECT_NEAR(r.paths[1].delay_s * kC / want, 1.0, 0.01) << dist_km << " km: " << want;
    // Weaker than |Gamma| times the direct path: divergence off a convex Earth.
    const double ratio = r.paths[1].amplitude / r.paths[0].amplitude;
    EXPECT_LE(ratio, 0.5);
    EXPECT_GT(ratio, 0.5 * (dist_km < 50 ? 0.99 : 0.3));
    // Carrier phase of a copy delayed by tau, mixed by the LO: pi - 2 pi f_lo tau.
    const double f_lo = kModeSCarrierHz * (1 + 3e-6);
    const double cyc = r.paths[1].delay_s * f_lo;
    EXPECT_NEAR(r.paths[1].phase_rad, M_PI - 2 * M_PI * (cyc - std::floor(cyc)), 1e-6);
  }
  // 200 km, 10 km, 10 m: about 0.765 m (a flat-ground model says ~1 m).
  Emitter far;
  far.lat = 40.0 + 200.0 / 111.0;
  far.lon = -88.0;
  far.alt_m = 10'000.0;
  EXPECT_NEAR(compute_link(site, far, cfg).paths[1].delay_s * kC, 0.765, 0.02);
}

TEST(IqLink, GroundBounceSpreadsOffTheConvexEarth) {
  // A 1,500 m antenna (a mountain top) and an aircraft 250 km out: the
  // reflection point is far from both, and the divergence factor matters.
  IqConfig cfg = clean_config();
  cfg.ground_reflection = 0.5;
  cfg.antenna_height_m = 1500.0;
  const double re = 4.0 / 3.0 * 6'371'000.0;
  Site site{40.0, -88.0, 0.0, 1500.0};
  Emitter e;
  e.lat = 40.0 + 250.0 / 111.0;
  e.lon = -88.0;
  e.alt_m = 3000.0;
  const LinkResult r = compute_link(site, e, cfg);
  ASSERT_EQ(r.n_paths, 2);
  double d1 = 0;
  brute_force_bounce(1500.0, 3000.0, r.ground_distance_m, re, &d1);
  const double d2 = r.ground_distance_m - d1;
  const double hr = 1500.0 - d1 * d1 / (2 * re), ht = 3000.0 - d2 * d2 / (2 * re);
  const double divergence = 1.0 / std::sqrt(1.0 + 2.0 * d1 * d2 / (re * (hr + ht)));
  EXPECT_LT(divergence, 0.7);
  EXPECT_NEAR(r.paths[1].amplitude / r.paths[0].amplitude, 0.5 * divergence, 0.005);
}

TEST(IqLink, NoGroundBounceBeyondTheHorizon) {
  IqConfig cfg = clean_config();
  cfg.ground_reflection = 0.5;
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter low;
  low.lat = 40.0 + 200'000.0 / 111'000.0;
  low.lon = -88.0;
  low.alt_m = 300.0;
  const LinkResult r = compute_link(site, low, cfg);
  EXPECT_GT(r.ground_distance_m, r.horizon_m);
  EXPECT_EQ(r.n_paths, 1);
}

TEST(IqLink, FixedReflectorAddsADelayedEcho) {
  IqConfig cfg = clean_config();
  cfg.reflectors.push_back({40.0, -88.0 + 3000.0 / 85'000.0, 50.0, 6.0});  // ~3 km east
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter e;
  e.lat = 40.5;
  e.lon = -88.0;
  e.alt_m = 9000.0;
  const LinkResult r = compute_link(site, e, cfg);
  ASSERT_EQ(r.n_paths, 2);
  // Echo delay is the extra path length: a few microseconds here.
  EXPECT_GT(r.paths[1].delay_s, 1e-6);
  EXPECT_LT(r.paths[1].delay_s, 20e-6);
  EXPECT_LT(r.paths[1].amplitude, r.paths[0].amplitude);
}

TEST(IqLink, EchoHasTheDopplerOfItsOwnPath) {
  // Receiver at 40 N; aircraft at 40.5 N flying north at 250 m/s, towards a
  // reflector at 41.5 N: the direct path recedes, the echo's first leg closes.
  IqConfig cfg = clean_config();
  cfg.diffraction_db_per_km = 0.0;
  cfg.reflectors.push_back({41.5, -88.0, 3000.0, 6.0});
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter e;
  e.lat = 40.5;
  e.lon = -88.0;
  e.alt_m = 3000.0;
  e.vel_north_mps = 250.0;
  const LinkResult r = compute_link(site, e, cfg);
  ASSERT_EQ(r.n_paths, 2);
  EXPECT_NEAR(r.paths[0].freq_hz, -909.0, 15.0);
  EXPECT_NEAR(r.paths[1].freq_hz, 909.0, 15.0);
}

TEST(IqLink, EchoFromBeyondTheReflectorsHorizonIsAttenuated) {
  IqConfig cfg = clean_config();
  Site site{40.0, -88.0, 0.0, 10.0};
  Emitter e;  // 300 m up, 300 km from a 20 m reflector next to the receiver
  e.lat = 40.0 + 300.0 / 111.0;
  e.lon = -88.0;
  e.alt_m = 300.0;
  cfg.reflectors.push_back({40.0, -88.0 + 1.0 / 85.0, 20.0, 6.0});
  const double with = compute_link(site, e, cfg).paths[1].amplitude;
  cfg.diffraction_db_per_km = 0.0;
  const double without = compute_link(site, e, cfg).paths[1].amplitude;
  EXPECT_LT(20 * std::log10(with / without), -100.0);
}

TEST(IqLink, ConfigValidation) {
  std::string why;
  EXPECT_TRUE(validate_iq_config(IqConfig{}, &why)) << why;
  auto bad = [&](auto mutate) {
    IqConfig c;
    mutate(c);
    return !validate_iq_config(c, &why) && !why.empty();
  };
  EXPECT_TRUE(bad([](IqConfig& c) { c.full_scale_dbm = -5000; }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.noise_figure_db = 1e300; }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.tail_s = 1e14; }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.sample_rate_hz = std::nan(""); }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.tx_power_max_dbm = c.tx_power_min_dbm - 1; }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.reflectors.push_back({40, -88, 100, -3}); }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.reflectors.assign(7, Reflector{}); }));
  EXPECT_TRUE(bad([](IqConfig& c) { c.oversample = 0; }));
}

// ---------------------------------------------------------------------------
// Waveform
// ---------------------------------------------------------------------------

class IqWaveform : public ::testing::TestWithParam<double> {};

TEST_P(IqWaveform, CarriesTheBitsInEveryFormat) {
  const double fs = GetParam();
  const SynthConfig syn = one_message_synth();
  const auto f = df17();
  const SynthOutput traffic = traffic_of({transmission(syn, f, 0.5, 30000)});
  IqConfig cfg = clean_config(fs);
  cfg.tx_offset_sd_hz = 300e3;  // a large carrier offset must not matter to magnitude slicing
  const IqPlan plan = plan_iq(syn, traffic, cfg);
  ASSERT_EQ(plan.messages.size(), 1u);
  const IqMessage& m = plan.messages[0];
  const auto iq = render_all(plan, cfg);
  EXPECT_EQ(slice(iq, m.sample_pos, fs, m.len), std::vector<std::uint8_t>(f.bytes, f.bytes + f.len));

  for (IqFormat fmt : {IqFormat::kCu8, IqFormat::kCs8, IqFormat::kCs16, IqFormat::kCf32}) {
    std::vector<std::uint8_t> q(plan.total_samples * iq_bytes_per_sample(fmt));
    EXPECT_EQ(quantize_iq(iq.data(), plan.total_samples, fmt, q.data()), 0u);
    const auto back = dequantize(q, fmt, plan.total_samples);
    EXPECT_EQ(slice(back, m.sample_pos, fs, m.len), std::vector<std::uint8_t>(f.bytes, f.bytes + f.len))
        << iq_format_name(fmt);
  }
}

INSTANTIATE_TEST_SUITE_P(Rates, IqWaveform, ::testing::Values(2.0e6, 2.4e6, 8.0e6, 12.0e6));

TEST(IqWaveform, PulseTimingAndAmplitude) {
  const double fs = 12e6;
  const SynthConfig syn = one_message_synth();
  const SynthOutput traffic = traffic_of({transmission(syn, df17(), 0.5, 30000)});
  const IqConfig cfg = clean_config(fs);
  const IqPlan plan = plan_iq(syn, traffic, cfg);
  const IqMessage& m = plan.messages[0];
  const auto iq = render_all(plan, cfg);
  const double a = m.link.paths[0].amplitude;
  // Preamble pulses at 0, 1, 3.5, 4.5 us; silence at 2, 3, 5.5-7.5 us.
  for (double t : {0.25, 1.25, 3.75, 4.75}) EXPECT_NEAR(mag(iq, m.sample_pos + t * 1e-6 * fs), a, a * 1e-3) << t;
  for (double t : {0.75, 2.0, 3.0, 5.5, 6.5, 7.5}) EXPECT_LT(mag(iq, m.sample_pos + t * 1e-6 * fs), a * 1e-6) << t;
  // Before and after the message: nothing.
  EXPECT_EQ(mag(iq, m.sample_pos - 5), 0.0);
  EXPECT_LT(mag(iq, m.sample_pos + 121e-6 * fs), a * 1e-6);
  // Arrival = transmit time + range / c.
  const double t_tx = 0.0002;
  EXPECT_NEAR(m.arrival_s, t_tx + m.link.range_m / kC, 1e-12);
  EXPECT_NEAR(m.sample_pos, m.arrival_s * fs, 1e-6);
}

TEST(IqWaveform, CarrierFrequencyIsWhatTheTruthSays) {
  for (double ppm : {0.0, 25.0}) {
    for (double fs : {12e6, 2.4e6}) {
      const SynthConfig syn = one_message_synth();
      Transmission tr = transmission(syn, df17(), 0.7, 35000);
      tr.ground_speed_kt = 480;  // towards the receiver: Doppler about +800 Hz
      tr.track_deg = 180;
      const SynthOutput traffic = traffic_of({tr});
      IqConfig cfg = clean_config(fs);
      cfg.tx_offset_sd_hz = 200e3;
      cfg.rx_ppm = ppm;
      const IqPlan plan = plan_iq(syn, traffic, cfg);
      ASSERT_EQ(plan.messages.size(), 1u);
      const IqMessage& m = plan.messages[0];
      EXPECT_GT(m.link.doppler_hz, 700.0);
      EXPECT_NEAR(m.observed_hz, (m.tx_offset_hz + m.link.doppler_hz - ppm * 1090.0) / (1 + ppm * 1e-6), 1e-6);
      EXPECT_NEAR(m.sample_pos, m.arrival_s * fs * (1 + ppm * 1e-6), 1e-6);
      const auto iq = render_all(plan, cfg);
      if (fs > 10e6) {
        // Only samples entirely inside a pulse: the phase advance is exact.
        EXPECT_NEAR(estimate_hz(iq, m, fs, 0.9999), m.observed_hz, 0.01) << "ppm " << ppm;
      } else {
        // At 2.4 MS/s every pulse touches an edge; the naive estimate is
        // biased by a few percent of the offset (a real estimator fits the
        // pulse shape).
        EXPECT_NEAR(estimate_hz(iq, m, fs), m.observed_hz, 0.03 * std::fabs(m.observed_hz) + 50) << "ppm " << ppm;
      }
    }
  }
}

TEST(IqWaveform, NoiseHasTheConfiguredPower) {
  SynthConfig syn;
  syn.duration_s = 0.2;
  const SynthOutput traffic = traffic_of({});
  IqConfig cfg;
  cfg.noise = true;
  const IqPlan plan = plan_iq(syn, traffic, cfg);
  EXPECT_NEAR(plan.noise_dbm, -174.0 + 10 * std::log10(2.4e6) + 5.0, 1e-9);
  const auto iq = render_all(plan, cfg);
  double sum[2] = {}, sq[2] = {};
  for (std::size_t i = 0; i < plan.total_samples; ++i) {
    for (int c = 0; c < 2; ++c) {
      const double v = static_cast<double>(iq[2 * i + static_cast<std::size_t>(c)]);
      sum[c] += v;
      sq[c] += v * v;
    }
  }
  const double n = static_cast<double>(plan.total_samples);
  for (int c = 0; c < 2; ++c) {
    EXPECT_NEAR(sum[c] / n, 0.0, plan.noise_sigma * 0.01);
    EXPECT_NEAR(std::sqrt(sq[c] / n) / plan.noise_sigma, 1.0, 0.01);
  }
}

TEST(IqWaveform, DcOffsetAndImbalance) {
  const SynthConfig syn = one_message_synth();
  const SynthOutput traffic = traffic_of({transmission(syn, df17(), 0.5, 30000)});
  IqConfig cfg = clean_config();
  cfg.tx_offset_sd_hz = 150e3;  // the carrier rotates, so I and Q both vary
  const IqPlan plan = plan_iq(syn, traffic, cfg);
  const auto ideal = render_all(plan, cfg);
  cfg.dc_i = 0.01;
  cfg.dc_q = -0.02;
  cfg.iq_gain_imbalance = 0.1;
  cfg.iq_phase_deg = 5.0;
  const auto bent = render_all(plan, cfg);
  const double s5 = std::sin(5.0 * M_PI / 180), c5 = std::cos(5.0 * M_PI / 180);
  double energy = 0;
  for (std::size_t i = 0; i < plan.total_samples; ++i) {
    const double ii = static_cast<double>(ideal[2 * i]), qq = static_cast<double>(ideal[2 * i + 1]);
    energy += ii * ii;
    ASSERT_NEAR(static_cast<double>(bent[2 * i]), ii + 0.01, 1e-6) << i;
    ASSERT_NEAR(static_cast<double>(bent[2 * i + 1]), 1.1 * (qq * c5 + ii * s5) - 0.02, 1e-6) << i;
  }
  EXPECT_GT(energy, 1e-3);  // there was a signal to distort
}

TEST(IqWaveform, CollisionsAreFlaggedAndAdd) {
  const SynthConfig syn = one_message_synth();
  Transmission a = transmission(syn, df17(), 0.5, 30000, 0.0002);
  Transmission b = transmission(syn, df17(), 0.55, 20000, 0.0002 + 30e-6);  // another aircraft, overlapping
  b.icao = 0x123456;
  Transmission c = transmission(syn, df17(), 0.5, 30000, 0.0012);  // alone
  c.icao = 0x654321;
  IqConfig cfg = clean_config();
  cfg.tx_offset_sd_hz = 50e3;
  const IqPlan plan = plan_iq(syn, traffic_of({a, b, c}), cfg);
  ASSERT_EQ(plan.messages.size(), 3u);
  EXPECT_TRUE(plan.messages[0].collided);
  EXPECT_TRUE(plan.messages[1].collided);
  EXPECT_FALSE(plan.messages[2].collided);
  EXPECT_EQ(plan.stats.collided, 2u);
  const double diff = plan.messages[1].snr_db - plan.messages[0].snr_db;
  EXPECT_GT(std::fabs(diff), 0.1);
  EXPECT_NEAR(plan.messages[0].interferer_db, diff, 1e-9);
  EXPECT_NEAR(plan.messages[1].interferer_db, -diff, 1e-9);
  EXPECT_EQ(plan.messages[2].interferer_db, -999.0);
  // Overlapping replies add: the scene is the sum of each message alone.
  SynthConfig longer = syn;
  const auto both = render_all(plan, cfg);
  const auto only_a = render_all(plan_iq(longer, traffic_of({a}), cfg), cfg);
  const auto only_b = render_all(plan_iq(longer, traffic_of({b}), cfg), cfg);
  const auto only_c = render_all(plan_iq(longer, traffic_of({c}), cfg), cfg);
  ASSERT_EQ(only_a.size(), both.size());
  ASSERT_EQ(only_b.size(), both.size());
  ASSERT_EQ(only_c.size(), both.size());
  for (std::size_t i = 0; i < both.size(); ++i) {
    ASSERT_NEAR(static_cast<double>(both[i]),
                static_cast<double>(only_a[i]) + static_cast<double>(only_b[i]) + static_cast<double>(only_c[i]), 1e-6);
  }
}

TEST(IqWaveform, SpansThatJustTouchCollide) {
  // Move b until its span starts exactly where a's ends: still a collision.
  const SynthConfig syn = one_message_synth();
  IqConfig cfg = clean_config(2e6);
  const Transmission a = transmission(syn, df17(), 0.5, 30000, 0.0002);
  bool found = false;
  for (int step = 0; step < 4000 && !found; ++step) {
    Transmission b = transmission(syn, df17(), 0.5, 30000, 0.0002 + 100e-6 + step * 50e-9);
    b.icao = 0x123456;
    const IqPlan plan = plan_iq(syn, traffic_of({a, b}), cfg);
    ASSERT_EQ(plan.messages.size(), 2u);
    if (plan.messages[1].first_sample == plan.messages[0].last_sample) {
      found = true;
      EXPECT_TRUE(plan.messages[0].collided);
      EXPECT_TRUE(plan.messages[1].collided);
    }
  }
  EXPECT_TRUE(found);
}

TEST(IqWaveform, MessagesAcrossAChunkBoundaryAreIntact) {
  // The same stationary message twice, an exact number of samples apart:
  // once inside a chunk, once straddling the boundary at kIqChunkSamples.
  const double fs = 2e6;  // 500 ns per sample: whole-sample shifts are whole ns
  SynthConfig syn;
  syn.duration_s = 0.05;
  IqConfig cfg = clean_config(fs);
  const Transmission a = transmission(syn, df17(), 0.5, 30000, 0.002);
  const IqPlan solo = plan_iq(syn, traffic_of({a}), cfg);
  const std::int64_t mid = (solo.messages[0].first_sample + solo.messages[0].last_sample) / 2;
  const std::int64_t shift = static_cast<std::int64_t>(kIqChunkSamples) - mid;
  // Same aircraft (same transmitter power), later transmission.
  const Transmission b = transmission(syn, df17(), 0.5, 30000, 0.002 + static_cast<double>(shift) / fs);
  const IqPlan plan = plan_iq(syn, traffic_of({a, b}), cfg);
  ASSERT_EQ(plan.messages.size(), 2u);
  const IqMessage& ma = plan.messages[0];
  const IqMessage& mb = plan.messages[1];
  ASSERT_LT(mb.first_sample, static_cast<std::int64_t>(kIqChunkSamples));
  ASSERT_GT(mb.last_sample, static_cast<std::int64_t>(kIqChunkSamples));
  ASSERT_EQ(mb.first_sample - ma.first_sample, shift);
  const auto iq = render_all(plan, cfg);
  auto m = [&](std::int64_t i) {
    const auto k = static_cast<std::size_t>(i);
    return std::hypot(static_cast<double>(iq[2 * k]), static_cast<double>(iq[2 * k + 1]));
  };
  double peak = 0;
  for (std::int64_t i = ma.first_sample; i <= ma.last_sample; ++i) {
    peak = std::max(peak, m(i));
    ASSERT_NEAR(m(i + shift), m(i), 1e-6) << i;
  }
  EXPECT_GT(peak, 0.01);
}

TEST(IqWaveform, EveryChunkHasItsOwnNoise) {
  SynthConfig syn;
  syn.duration_s = 0.1;
  IqConfig cfg;
  const IqPlan plan = plan_iq(syn, traffic_of({}), cfg);
  const auto iq = render_all(plan, cfg);
  double dotp = 0, n0 = 0, n1 = 0;
  for (std::size_t i = 0; i < 2 * kIqChunkSamples; ++i) {
    const double x = static_cast<double>(iq[i]), y = static_cast<double>(iq[i + 2 * kIqChunkSamples]);
    dotp += x * y;
    n0 += x * x;
    n1 += y * y;
  }
  EXPECT_LT(std::fabs(dotp) / std::sqrt(n0 * n1), 0.02);
}

TEST(IqWaveform, TheRecordingHoldsEveryMessage) {
  // A far aircraft transmitting at the very end: propagation (~1 ms) pushes
  // the reply past the nominal duration; the recording is extended for it.
  SynthConfig syn;
  syn.duration_s = 0.01;
  IqConfig cfg = clean_config();
  const Transmission t = transmission(syn, df17(), 2.3, 40000, 0.0099);
  const IqPlan plan = plan_iq(syn, traffic_of({t}), cfg);
  ASSERT_EQ(plan.messages.size(), 1u);
  EXPECT_GT(plan.messages[0].arrival_s, syn.duration_s);
  EXPECT_LT(static_cast<std::uint64_t>(plan.messages[0].last_sample), plan.total_samples);
}

TEST(IqWaveform, BeyondTheHorizonIsCounted) {
  const SynthConfig syn = one_message_synth();
  IqConfig cfg = clean_config();
  cfg.min_snr_db = -100;
  const IqPlan plan = plan_iq(syn, traffic_of({transmission(syn, df17(), 2.0, 1000)}), cfg);
  EXPECT_EQ(plan.stats.beyond_horizon, 1u);
}

TEST(IqWaveform, WeakMessagesAreSkipped) {
  const SynthConfig syn = one_message_synth();
  IqConfig cfg;
  cfg.min_snr_db = 200.0;
  const IqPlan plan = plan_iq(syn, traffic_of({transmission(syn, df17(), 0.5, 30000)}), cfg);
  EXPECT_TRUE(plan.messages.empty());
  EXPECT_EQ(plan.stats.too_weak, 1u);
}

// ---------------------------------------------------------------------------
// ADC and output
// ---------------------------------------------------------------------------

TEST(IqQuantize, FormatsAndClipping) {
  const float in[] = {0.0f, 0.0f, 1.0f, -1.0f, 1.5f, -0.25f, 0.5f, -2.0f};
  std::uint8_t q[8 * 4];
  EXPECT_EQ(quantize_iq(in, 4, IqFormat::kCu8, q), 2u);
  const std::uint8_t cu8[] = {128, 128, 255, 0, 255, 96, 191, 0};
  EXPECT_EQ(std::vector<std::uint8_t>(q, q + 8), std::vector<std::uint8_t>(cu8, cu8 + 8));
  EXPECT_EQ(quantize_iq(in, 4, IqFormat::kCs8, q), 2u);
  EXPECT_EQ(static_cast<std::int8_t>(q[2]), 127);
  EXPECT_EQ(static_cast<std::int8_t>(q[3]), -127);
  EXPECT_EQ(static_cast<std::int8_t>(q[7]), -128);
  EXPECT_EQ(quantize_iq(in, 4, IqFormat::kCs16, q), 2u);
  EXPECT_EQ(q[4], 0xFF);  // 32767 little-endian
  EXPECT_EQ(q[5], 0x7F);
  EXPECT_EQ(quantize_iq(in, 4, IqFormat::kCf32, q), 2u);
  float v;
  std::memcpy(&v, q + 16, 4);  // little-endian hosts only, as everywhere we build
  EXPECT_EQ(v, 1.5f);
  EXPECT_EQ(parse_iq_format("cu8"), IqFormat::kCu8);
  EXPECT_EQ(parse_iq_format("sc16"), IqFormat::kCs16);
  EXPECT_FALSE(parse_iq_format("cu16"));
  EXPECT_STREQ(sigmf_datatype(IqFormat::kCs16), "ci16_le");
}

// ---------------------------------------------------------------------------
// Whole scenarios
// ---------------------------------------------------------------------------

SynthConfig scenario_synth() {
  SynthConfig s;
  s.aircraft = 40;
  s.duration_s = 0.5;
  s.seed = 421;
  s.keep_transmissions = true;
  return s;
}

TEST(IqScenario, KeepingTransmissionsChangesNothingElse) {
  SynthConfig s = scenario_synth();
  s.keep_transmissions = false;
  const SynthOutput a = generate(s);
  s.keep_transmissions = true;
  const SynthOutput b = generate(s);
  ASSERT_EQ(a.records.size(), b.records.size());
  for (std::size_t i = 0; i < a.records.size(); ++i) {
    const auto& x = a.records[i];
    const auto& y = b.records[i];
    ASSERT_EQ(x.rx_ns, y.rx_ns);
    ASSERT_EQ(x.radio_ts, y.radio_ts);
    ASSERT_EQ(x.line, y.line);
    ASSERT_EQ(x.len, y.len);
    ASSERT_EQ(0, std::memcmp(x.payload, y.payload, x.len));
  }
  EXPECT_TRUE(a.transmissions.empty());
  EXPECT_EQ(b.transmissions.size(), b.stats.transmitted);
  for (std::size_t i = 1; i < b.transmissions.size(); ++i) {
    EXPECT_LE(b.transmissions[i - 1].tx_ns, b.transmissions[i].tx_ns);
  }
}

TEST(IqScenario, ChunksAreIndependentOfRenderOrder) {
  const SynthConfig s = scenario_synth();
  const SynthOutput traffic = generate(s);
  IqConfig cfg;
  cfg.ground_reflection = 0.4;
  const IqPlan plan = plan_iq(s, traffic, cfg);
  ASSERT_GT(plan.messages.size(), 100u);
  const auto forward = render_all(plan, cfg);
  // Render backwards: identical output (no state carried between chunks).
  std::vector<float> backward(forward.size());
  std::vector<float> chunk(2 * kIqChunkSamples);
  const std::uint64_t chunks = (plan.total_samples + kIqChunkSamples - 1) / kIqChunkSamples;
  for (std::uint64_t c = chunks; c-- > 0;) {
    const std::size_t n = render_iq_chunk(plan, cfg, c, chunk.data());
    std::memcpy(backward.data() + 2 * c * kIqChunkSamples, chunk.data(), 2 * n * sizeof(float));
  }
  EXPECT_EQ(0, std::memcmp(forward.data(), backward.data(), forward.size() * sizeof(float)));
}

TEST(IqScenario, EveryCleanStrongMessageCarriesItsBits) {
  const SynthConfig s = scenario_synth();
  const SynthOutput traffic = generate(s);
  IqConfig cfg;  // defaults: noise, dipole, ground bounce, Doppler, offsets
  cfg.full_scale_dbm = -50.0;
  const IqPlan plan = plan_iq(s, traffic, cfg);
  const auto iq = render_all(plan, cfg);
  int checked = 0;
  for (const IqMessage& m : plan.messages) {
    if (m.collided || m.snr_db < 20.0) continue;
    ++checked;
    EXPECT_EQ(slice(iq, m.sample_pos, cfg.sample_rate_hz, m.len), std::vector<std::uint8_t>(m.bytes, m.bytes + m.len))
        << std::hex << m.icao << " snr " << m.snr_db;
  }
  EXPECT_GT(checked, 50);
}

// Same bytes on every platform and compiler: the whole chain (traffic,
// transmitters, geometry, noise, ADC) is portable arithmetic. If this hash
// changes, the generator's output changed; update it only on purpose.
TEST(IqScenario, OutputHashIsStable) {
  const SynthConfig s = scenario_synth();
  const SynthOutput traffic = generate(s);
  IqConfig cfg;
  cfg.rx_ppm = 1.5;
  cfg.reflectors.push_back({s.receiver_lat + 0.02, s.receiver_lon, 300.0, 8.0});
  const IqPlan plan = plan_iq(s, traffic, cfg);
  const auto iq = render_all(plan, cfg);
  std::vector<std::uint8_t> q(plan.total_samples * 2);
  quantize_iq(iq.data(), plan.total_samples, IqFormat::kCu8, q.data());
  const std::string meta = sigmf_meta(s, cfg, plan);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%08x %08x %zu", adsb::crc32(q.data(), q.size()),
                adsb::crc32(meta.data(), meta.size()), plan.messages.size());
  EXPECT_STREQ(buf, "c29a3160 38156009 115");
}

TEST(IqScenario, SigmfMetadataHasTheRequiredFields) {
  const SynthConfig s = scenario_synth();
  const SynthOutput traffic = generate(s);
  const IqConfig cfg;
  const IqPlan plan = plan_iq(s, traffic, cfg);
  const std::string meta = sigmf_meta(s, cfg, plan);
  for (const char* key : {"\"core:datatype\": \"cu8\"", "\"core:sample_rate\": 2400000.000", "\"core:version\": \"1.0.0\"",
                          "\"captures\"", "\"core:frequency\": 1090000000.000", "\"annotations\"", "\"adsb:transmitters\""}) {
    EXPECT_NE(meta.find(key), std::string::npos) << key;
  }
  std::size_t count = 0;
  for (std::size_t p = 0; (p = meta.find("\"adsb:hex\"", p)) != std::string::npos; ++p) ++count;
  EXPECT_EQ(count, plan.messages.size());
}

}  // namespace
