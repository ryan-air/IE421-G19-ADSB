// Carrier frequency estimation (adsb/demod/carrier.hpp), against the IQ
// generator's truth (plan §22.5).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "adsb/common/portable_math.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/demod/carrier.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/modes/encoder.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb::sim;
using adsb::demod::CarrierConfig;
using adsb::demod::CarrierEstimate;
using adsb::demod::CarrierEstimator;

adsb::modes::Frame df17(std::uint32_t icao = 0xABCDEF, double lat = 40.5) {
  const auto cpr = adsb::modes::cpr_encode(lat, -88.3, false, false);
  return adsb::modes::encode_airborne_position(icao, 30000, cpr);
}

Transmission transmission(const SynthConfig& s, const adsb::modes::Frame& f, double t_s,
                          std::uint32_t icao = 0xABCDEF) {
  Transmission tr;
  tr.tx_ns = s.start_rx_ns + static_cast<std::uint64_t>(t_s * 1e9);
  tr.icao = icao;
  tr.len = f.len;
  std::memcpy(tr.bytes, f.bytes, f.len);
  tr.lat = s.receiver_lat + 0.5;
  tr.lon = s.receiver_lon;
  tr.alt_ft = 30000;
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

IqConfig clean_config(double rate) {
  IqConfig c;
  c.sample_rate_hz = rate;
  c.noise = false;
  c.ground_reflection = 0.0;
  c.antenna = AntennaPattern::kOmni;
  c.tx_offset_sd_hz = 0.0;
  c.tx_drift_sd_hz_per_s = 0.0;
  c.tx_jitter_sd_hz = 0.0;
  c.full_scale_dbm = -40.0;
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

// One message per offset, 300 us apart, rendered and estimated at the true position.
struct Rendered {
  IqPlan plan;
  std::vector<float> iq;
};

Rendered render(const std::vector<double>& offsets, IqConfig cfg, std::uint64_t seed = 1) {
  SynthConfig syn;
  syn.duration_s = 0.0005 + 0.0003 * static_cast<double>(offsets.size());
  std::vector<Transmission> txs;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto icao = static_cast<std::uint32_t>(0x100000 + i);
    txs.push_back(transmission(syn, df17(icao), 0.0002 + 0.0003 * static_cast<double>(i), icao));
  }
  cfg.seed = seed;
  Rendered r;
  r.plan = plan_iq(syn, traffic_of(txs), cfg);
  // Set each message's carrier directly (the transmitter draw is per
  // aircraft from the seed; the tests want chosen offsets).
  for (auto& m : r.plan.messages) {
    const std::size_t i = m.icao - 0x100000;
    const double shift = offsets[i] - m.observed_hz;
    m.observed_hz += shift;
    for (int p = 0; p < m.link.n_paths; ++p) m.link.paths[p].freq_hz += shift;
  }
  r.iq = render_all(r.plan, cfg);
  return r;
}

TEST(Carrier, PulseIntervalsFollowThePpmBits) {
  double a[116], b[116];
  const auto f = df17();
  const int n = CarrierEstimator::pulse_intervals_us(f.bytes, f.len, a, b);
  ASSERT_EQ(n, 4 + 112);
  EXPECT_EQ(a[0], 0.0);
  EXPECT_EQ(a[1], 1.0);
  EXPECT_EQ(a[2], 3.5);
  EXPECT_EQ(a[3], 4.5);
  for (int i = 0; i < 112; ++i) {
    const bool one = (f.bytes[i / 8] >> (7 - i % 8)) & 1;
    EXPECT_EQ(a[4 + i], 8.0 + i + (one ? 0.0 : 0.5));
    EXPECT_EQ(b[4 + i] - a[4 + i], 0.5);
  }
}

TEST(Carrier, NoiseFreeEstimateIsExactAtEveryRate) {
  const std::vector<double> offsets = {-950e3, -400e3, -150e3, -1234.5, 0.0, 37e3, 400e3, 950e3};
  for (double fs : {2.0e6, 2.4e6, 6e6, 12e6}) {
    std::vector<double> use;
    for (double o : offsets)
      if (std::fabs(o) < 0.45 * fs) use.push_back(o);
    const Rendered r = render(use, clean_config(fs));
    CarrierConfig cc;
    cc.sample_rate_hz = fs;
    CarrierEstimator est(cc);
    ASSERT_EQ(r.plan.messages.size(), use.size());
    for (const auto& m : r.plan.messages) {
      const CarrierEstimate e = est.estimate(r.iq.data(), 0, r.plan.total_samples, m.sample_pos, m.bytes, m.len);
      ASSERT_TRUE(e.ok) << fs;
      // Pulse edges ramp (rise/fall), so partly covered samples are not
      // exactly at their covered middle: a few ppm of the offset.
      EXPECT_NEAR(e.freq_hz, m.observed_hz, 1.0 + 5e-6 * std::fabs(m.observed_hz))
          << "fs " << fs << " offset " << m.observed_hz << " coarse "
                                                  << e.coarse_hz;
      EXPECT_GT(e.coherence, 0.999);
      EXPECT_LT(e.phase_rms_rad, 0.02);
    }
  }
}

double gaussian(adsb::Rng& r) {
  const double u1 = 1.0 - r.uniform();
  const double u2 = r.uniform();
  return std::sqrt(-2.0 * adsb::pm::log(u1)) * adsb::pm::cos(adsb::pm::kTwoPi * u2);
}

// Adds complex Gaussian noise so that each message's direct path has the given
// SNR (pulse power over noise power, both components).
void add_noise(Rendered& r, double snr_db, std::uint64_t seed) {
  const double amp = r.plan.messages.at(0).link.paths[0].amplitude;
  const double sigma = amp / std::sqrt(2.0 * std::pow(10.0, snr_db / 10.0));
  adsb::Rng rng(seed);
  for (auto& v : r.iq) v += static_cast<float>(sigma * gaussian(rng));
}

// The reported standard error matches the actual error under noise, and
// the error is what the Cramer-Rao bound allows (within a factor).
TEST(Carrier, StandardErrorMatchesTheErrorUnderNoise) {
  for (double fs : {2.4e6, 6e6}) {
    for (double snr : {10.0, 20.0, 30.0}) {
      adsb::Rng rng(static_cast<std::uint64_t>(snr * 1000 + fs / 1e5));
      std::vector<double> offsets;
      for (int i = 0; i < 150; ++i) offsets.push_back(rng.uniform(-300e3, 300e3));
      Rendered r = render(offsets, clean_config(fs));
      add_noise(r, snr, rng.next());
      CarrierConfig cc;
      cc.sample_rate_hz = fs;
      CarrierEstimator est(cc);
      double z2 = 0.0, e2 = 0.0, coh = 0.0;
      int n = 0;
      for (const auto& m : r.plan.messages) {
        const CarrierEstimate e = est.estimate(r.iq.data(), 0, r.plan.total_samples, m.sample_pos, m.bytes, m.len);
        ASSERT_TRUE(e.ok);
        const double err = e.freq_hz - m.observed_hz;
        z2 += (err / e.sigma_hz) * (err / e.sigma_hz);
        e2 += err * err;
        coh += e.coherence;
        ++n;
      }
      const double z_rms = std::sqrt(z2 / n);
      const double e_rms = std::sqrt(e2 / n);
      // CRB for a tone of N samples over span T: sigma_f ~ sqrt(12) / (2 pi T sqrt(N SNR)) (rough: the
      // samples are not uniform in time).
      const double nsamp = 115.0 * fs / 1e6 * 0.5;
      const double crb = std::sqrt(12.0) / (adsb::pm::kTwoPi * 120e-6 * std::sqrt(nsamp * std::pow(10.0, snr / 10.0)));
      EXPECT_GT(z_rms, 0.75) << "fs " << fs << " snr " << snr;
      EXPECT_LT(z_rms, 1.35) << "fs " << fs << " snr " << snr;
      EXPECT_LT(e_rms, 2.0 * crb) << "fs " << fs << " snr " << snr << " rms " << e_rms << " crb " << crb;
      EXPECT_GT(coh / n, snr >= 20 ? 0.95 : 0.7);
    }
  }
}

// A transmitter whose phase jumps at every pulse (not phase-coherent) has no
// usable carrier across the message: coherence says so.
TEST(Carrier, CoherenceIsLowWhenThePhaseJumpsBetweenPulses) {
  const double fs = 2.4e6;
  Rendered r = render({120e3, -60e3}, clean_config(fs));
  CarrierConfig cc;
  cc.sample_rate_hz = fs;
  CarrierEstimator est(cc);
  const IqMessage& coherent = r.plan.messages[0];
  const IqMessage& jumpy = r.plan.messages[1];
  double a[116], b[116];
  const int np = CarrierEstimator::pulse_intervals_us(jumpy.bytes, jumpy.len, a, b);
  adsb::Rng rng(7);
  std::vector<double> phase(static_cast<std::size_t>(np));
  for (auto& p : phase) p = adsb::pm::kTwoPi * rng.uniform();
  for (auto k = jumpy.first_sample; k <= jumpy.last_sample; ++k) {
    const double t_us = (static_cast<double>(k) - jumpy.sample_pos) / 2.4;
    int q = -1;
    for (int i = 0; i < np; ++i)
      if (t_us >= a[i] - 0.25 && t_us < b[i] + 0.25) q = i;
    if (q < 0) continue;
    double s, c;
    adsb::pm::sincos(phase[static_cast<std::size_t>(q)], s, c);
    const auto u = static_cast<std::size_t>(k);
    const double i0 = static_cast<double>(r.iq[2 * u]), q0 = static_cast<double>(r.iq[2 * u + 1]);
    r.iq[2 * u] = static_cast<float>(i0 * c - q0 * s);
    r.iq[2 * u + 1] = static_cast<float>(i0 * s + q0 * c);
  }
  const CarrierEstimate good = est.estimate(r.iq.data(), 0, r.plan.total_samples, coherent.sample_pos, coherent.bytes,
                                            coherent.len);
  const CarrierEstimate bad = est.estimate(r.iq.data(), 0, r.plan.total_samples, jumpy.sample_pos, jumpy.bytes,
                                           jumpy.len);
  ASSERT_TRUE(good.ok);
  EXPECT_GT(good.coherence, 0.999);
  EXPECT_LT(bad.coherence, 0.5);
  EXPECT_GT(bad.phase_rms_rad, 1.0);
}

TEST(Carrier, ShortFramesAndBounds) {
  const double fs = 2.4e6;
  SynthConfig syn;
  syn.duration_s = 0.001;
  const auto f = adsb::modes::encode_df11(0x123456, 5);
  Transmission tr = transmission(syn, f, 0.0002, 0x123456);
  IqConfig cfg = clean_config(fs);
  IqPlan plan = plan_iq(syn, traffic_of({tr}), cfg);
  ASSERT_EQ(plan.messages.size(), 1u);
  auto& m = plan.messages[0];
  const double shift = 250e3 - m.observed_hz;
  m.observed_hz += shift;
  m.link.paths[0].freq_hz += shift;
  const auto iq = render_all(plan, cfg);
  CarrierConfig cc;
  cc.sample_rate_hz = fs;
  CarrierEstimator est(cc);
  const CarrierEstimate e = est.estimate(iq.data(), 0, plan.total_samples, m.sample_pos, m.bytes, m.len);
  ASSERT_TRUE(e.ok);
  EXPECT_EQ(m.len, 7);
  EXPECT_NEAR(e.freq_hz, 250e3, 1.0 + 2e-5 * 250e3);  // a shorter span: more edge-model error
  // The same samples handed over from another base: the result is identical.
  const auto first = static_cast<std::int64_t>(m.first_sample) - 3;
  const CarrierEstimate e2 = est.estimate(iq.data() + 2 * first, first,
                                          plan.total_samples - static_cast<std::size_t>(first),
                                          m.sample_pos, m.bytes, m.len);
  ASSERT_TRUE(e2.ok);
  EXPECT_EQ(e2.freq_hz, e.freq_hz);
  // Not wholly inside the samples: no estimate.
  EXPECT_FALSE(
      est.estimate(iq.data(), 0, static_cast<std::size_t>(m.last_sample - 5), m.sample_pos, m.bytes, m.len).ok);
  EXPECT_FALSE(est.estimate(iq.data() + 2 * (first + 10), first + 10, 1000, m.sample_pos, m.bytes, m.len).ok);
  EXPECT_FALSE(est.estimate(iq.data(), 0, plan.total_samples, m.sample_pos, m.bytes, 5).ok);
  EXPECT_FALSE(est.estimate(iq.data(), 0, plan.total_samples, std::nan(""), m.bytes, m.len).ok);
  // Silence: no estimate rather than a number.
  std::vector<float> zeros(iq.size(), 0.0f);
  EXPECT_FALSE(est.estimate(zeros.data(), 0, plan.total_samples, m.sample_pos, m.bytes, m.len).ok);
}

// Clipped samples (and those a caller marks non-finite) are left out and
// counted; the estimate from the rest stays right.
TEST(Carrier, ClippedAndMarkedSamplesAreLeftOut) {
  const double fs = 2.4e6;
  Rendered r = render({180e3}, clean_config(fs));
  const IqMessage& m = r.plan.messages[0];
  CarrierConfig cc;
  cc.sample_rate_hz = fs;
  CarrierEstimator est(cc);
  const CarrierEstimate clean = est.estimate(r.iq.data(), 0, r.plan.total_samples, m.sample_pos, m.bytes, m.len);
  ASSERT_TRUE(clean.ok);
  EXPECT_EQ(clean.clipped, 0);
  // Every 5th pulse sample: clip one component to full scale (which bends
  // the phase), mark every 7th as unusable.
  int clipped = 0, marked = 0;
  for (auto k = m.first_sample; k <= m.last_sample; ++k) {
    const auto u = static_cast<std::size_t>(k);
    if (std::hypot(r.iq[2 * u], r.iq[2 * u + 1]) < 0.5f * static_cast<float>(m.link.paths[0].amplitude)) continue;
    if (k % 5 == 0) {
      r.iq[2 * u] = r.iq[2 * u] > 0 ? 1.0f : -1.0f;
      ++clipped;
    } else if (k % 7 == 0) {
      r.iq[2 * u + 1] = std::nanf("");
      ++marked;
    }
  }
  ASSERT_GT(clipped, 10);
  ASSERT_GT(marked, 5);
  const CarrierEstimate e = est.estimate(r.iq.data(), 0, r.plan.total_samples, m.sample_pos, m.bytes, m.len);
  ASSERT_TRUE(e.ok);
  EXPECT_GE(e.clipped, clipped + marked - 4);  // a few may fall below min_cover
  EXPECT_EQ(e.samples + e.clipped, clean.samples);
  EXPECT_NEAR(e.freq_hz, m.observed_hz, 1.0 + 5e-6 * 180e3);
  EXPECT_TRUE(std::isfinite(e.sigma_hz));
  // With clipping not detected (clip_level > 1), the clipped samples are used and bend the estimate.
  CarrierConfig open = cc;
  open.clip_level = 2.0;
  CarrierEstimator est2(open);
  const CarrierEstimate bent = est2.estimate(r.iq.data(), 0, r.plan.total_samples, m.sample_pos, m.bytes, m.len);
  ASSERT_TRUE(bent.ok);
  EXPECT_GT(std::fabs(bent.freq_hz - m.observed_hz), 5.0 * std::fabs(e.freq_hz - m.observed_hz) + 2.0);
}

// Sparse junk (a few nonzero samples among zeros) can let a few samples
// dominate the weights; the fit must not then wander off and still claim
// an estimate (found by review: f = -fs with sigma 0).
TEST(Carrier, JunkNeverGivesAWildEstimate) {
  adsb::Rng rng(1);
  const double rates[2] = {2.0e6, 2.4e6};
  int ok = 0;
  for (int it = 0; it < 4000; ++it) {
    CarrierConfig c;
    c.sample_rate_hz = rates[it % 2];
    c.min_cover = (it & 2) ? 0.9 : 0.5;
    CarrierEstimator est(c);
    std::uint8_t b[14];
    for (auto& x : b) x = static_cast<std::uint8_t>(rng.next());
    const int len = (it & 4) ? 7 : 14;
    const auto n = static_cast<std::size_t>(130 * c.sample_rate_hz / 1e6) + 20;
    std::vector<float> iq(2 * n, 0.0f);
    const std::uint64_t dens = 2 + rng.below(40);
    for (auto& v : iq)
      if (rng.below(dens) == 0) v = static_cast<float>(rng.uniform(-1.0, 1.0));
    const CarrierEstimate e = est.estimate(iq.data(), 0, n, 4.0, b, len);
    if (!e.ok) continue;
    ++ok;
    ASSERT_TRUE(std::isfinite(e.freq_hz) && std::isfinite(e.sigma_hz)) << it;
    // Within a few FFT bins of the coarse peak, and inside the band.
    ASSERT_LE(std::fabs(e.freq_hz - e.coarse_hz), 0.05 * c.sample_rate_hz) << it << " f " << e.freq_hz;
    ASSERT_LE(std::fabs(e.freq_hz), 0.55 * c.sample_rate_hz) << it << " f " << e.freq_hz;
    ASSERT_GE(e.sigma_hz, 0.0) << it;
  }
  EXPECT_GT(ok, 100);
}

TEST(Carrier, ToComplexScalesEveryFormat) {
  const std::uint8_t cu8[4] = {255, 0, 127, 128};
  float out[4];
  adsb::demod::to_complex(cu8, 2, adsb::demod::SampleFormat::kCu8, out);
  EXPECT_FLOAT_EQ(out[0], 1.0f);
  EXPECT_FLOAT_EQ(out[1], -1.0f);
  EXPECT_NEAR(out[2], -0.5f / 127.5f, 1e-7);
  EXPECT_NEAR(out[3], 0.5f / 127.5f, 1e-7);
  const std::uint8_t cs8[2] = {0x80, 0x40};
  adsb::demod::to_complex(cs8, 1, adsb::demod::SampleFormat::kCs8, out);
  EXPECT_FLOAT_EQ(out[0], -1.0f);
  EXPECT_FLOAT_EQ(out[1], 0.5f);
  const std::uint8_t cs16[4] = {0x00, 0x80, 0x00, 0x40};
  adsb::demod::to_complex(cs16, 1, adsb::demod::SampleFormat::kCs16, out);
  EXPECT_FLOAT_EQ(out[0], -1.0f);
  EXPECT_FLOAT_EQ(out[1], 0.5f);
  const float cf[2] = {0.25f, NAN};
  std::uint8_t raw[8];
  std::memcpy(raw, cf, 8);
  adsb::demod::to_complex(raw, 1, adsb::demod::SampleFormat::kCf32, out);
  EXPECT_FLOAT_EQ(out[0], 0.25f);
  EXPECT_EQ(out[1], 0.0f);
}

TEST(Carrier, ConfigValidation) {
  CarrierConfig c;
  EXPECT_TRUE(validate(c));
  c.sample_rate_hz = 0.5e6;
  EXPECT_FALSE(validate(c));
  c = {};
  c.min_cover = 0.0;
  EXPECT_FALSE(validate(c));
  c = {};
  c.fine_iterations = 0;
  EXPECT_FALSE(validate(c));
  c = {};
  c.min_samples = 2;
  EXPECT_FALSE(validate(c));
}

// The demodulator's own position (not the truth) is good enough: on a busy,
// noisy scenario with transmitter drift, Doppler and a receiver ppm error,
// estimates from demodulated frames match the generator's observed
// frequency.
TEST(Carrier, FromDemodulatedFramesOfARealisticScenario) {
  SynthConfig syn;
  syn.aircraft = 40;
  syn.duration_s = 3.0;
  syn.seed = 11;
  syn.keep_transmissions = true;
  const SynthOutput traffic = generate(syn);
  IqConfig cfg;
  cfg.seed = 5;
  cfg.rx_ppm = 30.0;
  cfg.sample_rate_hz = 2.4e6;
  const IqPlan plan = plan_iq(syn, traffic, cfg);
  const auto iq = render_all(plan, cfg);
  std::vector<std::uint8_t> q(2 * plan.total_samples);
  (void)quantize_iq(iq.data(), plan.total_samples, IqFormat::kCu8, q.data());
  std::vector<float> fq(2 * plan.total_samples);
  adsb::demod::to_complex(q.data(), plan.total_samples, adsb::demod::SampleFormat::kCu8, fq.data());

  adsb::demod::DemodConfig dc;
  dc.sample_rate_hz = cfg.sample_rate_hz;
  adsb::demod::Demodulator demod(dc);
  std::vector<adsb::demod::DemodFrame> frames;
  for (std::size_t off = 0; off < plan.total_samples; off += dc.block_samples) {
    const std::size_t n = std::min(dc.block_samples, static_cast<std::size_t>(plan.total_samples) - off);
    for (const auto& f : demod.feed(q.data() + 2 * off, n, adsb::demod::SampleFormat::kCu8)) frames.push_back(f);
  }
  for (const auto& f : demod.flush()) frames.push_back(f);
  ASSERT_GT(frames.size(), 200u);

  CarrierConfig cc;
  cc.sample_rate_hz = cfg.sample_rate_hz;
  CarrierEstimator est(cc);
  int matched = 0, good = 0, not_ok = 0;
  for (const auto& f : frames) {
    // The truth for this frame: same bytes, position within 2 samples.
    const IqMessage* t = nullptr;
    for (const auto& m : plan.messages)
      if (std::fabs(m.sample_pos - f.sample_pos) < 2.0 && std::memcmp(m.bytes, f.bytes, f.len) == 0) t = &m;
    if (!t || t->collided) continue;
    const CarrierEstimate e = est.estimate(fq.data(), 0, plan.total_samples, f);
    if (!e.ok) {  // every pulse sample clipped (a strong, close aircraft)
      ++not_ok;
      continue;
    }
    ++matched;
    // Within 4 standard errors (plus the edge-model term) of the truth.
    if (std::fabs(e.freq_hz - t->observed_hz) < 4.0 * e.sigma_hz + 5e-6 * std::fabs(t->observed_hz) + 1.0) ++good;
  }
  ASSERT_GT(matched, 500);
  EXPECT_LT(not_ok, matched / 20);
  EXPECT_GE(good, matched * 99 / 100) << good << " of " << matched;
}

}  // namespace
