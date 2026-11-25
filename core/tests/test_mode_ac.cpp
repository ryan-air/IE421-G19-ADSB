// Tests for Mode A/C replies: the code layout and Gillham encoder
// (modes/mode_ac.hpp, modes/altitude.hpp), the generator's replies
// (IqConfig::modeac_*), the demodulator's detection (DemodConfig::mode_ac)
// and what downstream code does with a 2-byte frame.
//
// The demodulator tests render their own replies (rectangular pulses with
// exact sample overlap, a carrier, Gaussian noise), independent of the
// scenario generator; the scenario tests at the end use the generator.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "adsb/common/hash.hpp"
#include "adsb/common/portable_math.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/ingest/arbiter.hpp"
#include "adsb/ingest/beast.hpp"
#include "adsb/ingest/iq_source.hpp"
#include "adsb/modes/altitude.hpp"
#include "adsb/modes/encoder.hpp"
#include "adsb/modes/mode_ac.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"
#include "adsb/state/track_engine.hpp"

namespace {

using namespace adsb::demod;
using adsb::modes::feet_to_gillham;
using adsb::modes::gillham_to_feet;
using adsb::modes::mode_ac_code;
using adsb::modes::mode_ac_pulses;

// ---------------------------------------------------------------------------
// Layout and Gillham code
// ---------------------------------------------------------------------------

std::vector<std::uint16_t> all_codes() {
  std::vector<std::uint16_t> v;
  for (std::uint32_t c = 0; c <= 0xFFFF; ++c) {
    if ((c & ~std::uint32_t{adsb::modes::kModeAcCodeMask}) == 0) v.push_back(static_cast<std::uint16_t>(c));
  }
  return v;
}

TEST(ModeAcLayout, GillhamEncoderInvertsTheDecoderOverItsWholeRange) {
  // Every code the decoder accepts comes back from the encoder, and every
  // 100 ft step from -1200 to 126700 ft has exactly one code.
  std::set<std::int32_t> seen;
  for (std::uint32_t c = 0; c <= 0x7777; ++c) {
    const auto code = static_cast<std::uint16_t>(c);
    if ((code & 0x8888u) != 0) continue;
    const auto ft = gillham_to_feet(code);
    if (!ft) continue;
    EXPECT_TRUE(seen.insert(*ft).second) << "two codes for " << *ft;
    const auto back = feet_to_gillham(*ft);
    ASSERT_TRUE(back.has_value()) << *ft;
    EXPECT_EQ(*back, code) << *ft;
  }
  EXPECT_EQ(seen.size(), 1280u);
  for (std::int32_t alt = -1200; alt <= 126700; alt += 100) {
    const auto code = feet_to_gillham(alt);
    ASSERT_TRUE(code.has_value()) << alt;
    EXPECT_EQ(gillham_to_feet(*code), alt);
    EXPECT_EQ(*code & 0x0001u, 0u) << "D1 is never used";
  }
}

TEST(ModeAcLayout, GillhamRoundsToTheNearest100Feet) {
  EXPECT_EQ(gillham_to_feet(*feet_to_gillham(949)), 900);
  EXPECT_EQ(gillham_to_feet(*feet_to_gillham(950)), 1000);
  EXPECT_EQ(gillham_to_feet(*feet_to_gillham(-1149)), -1100);
  EXPECT_EQ(gillham_to_feet(*feet_to_gillham(-1250)), -1200);
  EXPECT_FALSE(feet_to_gillham(-1251).has_value());
  EXPECT_EQ(gillham_to_feet(*feet_to_gillham(126749)), 126700);
  EXPECT_FALSE(feet_to_gillham(126750).has_value());
}

TEST(ModeAcLayout, PulseSlotsMatchReadsbsBitAssembly) {
  // readsb demodulate2400AC reads 20 bits, bit k of the reply at mask
  // 1 << (19 - k), requires framing 0x80020 and quiet 0x0101B, and builds
  // the code word with this table (written out here independently of
  // mode_ac.hpp).
  const std::pair<std::uint32_t, std::uint16_t> readsb[] = {
      {0x40000, 0x0010}, {0x20000, 0x1000}, {0x10000, 0x0020}, {0x08000, 0x2000}, {0x04000, 0x0040},
      {0x02000, 0x4000}, {0x00800, 0x0100}, {0x00400, 0x0001}, {0x00200, 0x0200}, {0x00100, 0x0002},
      {0x00080, 0x0400}, {0x00040, 0x0004}, {0x00004, 0x0080}};
  for (const std::uint16_t code : all_codes()) {
    std::uint32_t bits = 0x80020;
    for (const auto& [in, out] : readsb) {
      if (code & out) bits |= in;
    }
    std::uint32_t ours = 0;
    const std::uint32_t p = mode_ac_pulses(code);
    for (int k = 0; k < 20; ++k) {
      if (p >> k & 1u) ours |= 1u << (19 - k);
    }
    ASSERT_EQ(ours, bits) << std::hex << code;
    EXPECT_EQ(bits & 0x0101Bu, 0u);
    EXPECT_EQ(mode_ac_code(p), code);
  }
  // Annex 10 order: F1 C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4 F2, SPI at 24.65 us.
  EXPECT_EQ(mode_ac_pulses(0x0000), 1u | 1u << 14);
  EXPECT_EQ(mode_ac_pulses(0x4000), 1u | 1u << 6 | 1u << 14);   // A4
  EXPECT_EQ(mode_ac_pulses(0x0001), 1u | 1u << 9 | 1u << 14);   // D1
  EXPECT_EQ(mode_ac_pulses(0x0080), 1u | 1u << 14 | 1u << 17);  // SPI
  EXPECT_DOUBLE_EQ(adsb::modes::kModeAcF2Us, 20.3);
  EXPECT_DOUBLE_EQ(adsb::modes::kModeAcSpiSlot * adsb::modes::kModeAcSlotUs - adsb::modes::kModeAcF2Us, 4.35);
  EXPECT_DOUBLE_EQ(adsb::modes::mode_ac_duration_us(0x7700), 20.75);
  EXPECT_DOUBLE_EQ(adsb::modes::mode_ac_duration_us(0x7780), 25.1);
}

// ---------------------------------------------------------------------------
// An independent renderer
// ---------------------------------------------------------------------------

struct Pulse {
  double a_us, b_us;
};

std::vector<Pulse> reply_pulses(std::uint16_t code, std::uint32_t extra_slots = 0) {
  std::vector<Pulse> p;
  const std::uint32_t mask = mode_ac_pulses(code) | extra_slots;
  for (int k = 0; k < 20; ++k) {
    if (mask >> k & 1u) p.push_back({1.45 * k, 1.45 * k + 0.45});
  }
  return p;
}

std::vector<Pulse> mode_s_pulses(const adsb::modes::Frame& f) {
  std::vector<Pulse> p = {{0, 0.5}, {1, 1.5}, {3.5, 4}, {4.5, 5}};
  for (int i = 0; i < f.len * 8; ++i) {
    const bool one = (f.bytes[i / 8] >> (7 - i % 8)) & 1;
    const double a = 8.0 + i + (one ? 0.0 : 0.5);
    p.push_back({a, a + 0.5});
  }
  return p;
}

struct Burst {
  std::vector<Pulse> pulses;
  double start_us = 0.0;
  double amp = 0.3;
  double phase = 0.0;
  double freq_hz = 0.0;
};

// Interleaved I/Q floats: sample k covers [(k - 0.5) / fs, (k + 0.5) / fs).
std::vector<float> render(const std::vector<Burst>& bursts, double fs, std::size_t n, double noise_sigma,
                          std::uint64_t seed = 1) {
  std::vector<double> iq(2 * n, 0.0);
  const double us = fs / 1e6;
  for (const Burst& b : bursts) {
    for (const Pulse& p : b.pulses) {
      const double xa = (b.start_us + p.a_us) * us, xb = (b.start_us + p.b_us) * us;
      const auto k0 = static_cast<std::ptrdiff_t>(std::floor(xa + 0.5));
      const auto k1 = static_cast<std::ptrdiff_t>(std::floor(xb + 0.5));
      for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(k0, 0); k <= k1 && k < static_cast<std::ptrdiff_t>(n); ++k) {
        const double lo = std::max(xa, static_cast<double>(k) - 0.5), hi = std::min(xb, static_cast<double>(k) + 0.5);
        if (hi <= lo) continue;
        const double ph = b.phase + 2 * adsb::pm::kPi * b.freq_hz * static_cast<double>(k) / fs;
        iq[2 * static_cast<std::size_t>(k)] += b.amp * (hi - lo) * std::cos(ph);
        iq[2 * static_cast<std::size_t>(k) + 1] += b.amp * (hi - lo) * std::sin(ph);
      }
    }
  }
  adsb::Rng rng(seed);
  std::vector<float> out(2 * n);
  for (std::size_t i = 0; i < 2 * n; i += 2) {
    double g0 = 0, g1 = 0;
    if (noise_sigma > 0) {
      const double u1 = 1.0 - rng.uniform(), u2 = rng.uniform();
      const double r = std::sqrt(-2.0 * adsb::pm::log(u1)) * noise_sigma;
      double s, c;
      adsb::pm::sincos(2 * adsb::pm::kPi * u2, s, c);
      g0 = r * c;
      g1 = r * s;
    }
    out[i] = static_cast<float>(iq[i] + g0);
    out[i + 1] = static_cast<float>(iq[i + 1] + g1);
  }
  return out;
}

DemodConfig ac_config(double fs) {
  DemodConfig c;
  c.sample_rate_hz = fs;
  c.mode_ac = true;
  return c;
}

std::vector<DemodFrame> demod_floats(const std::vector<float>& iq, const DemodConfig& cfg, std::size_t chunk = 0,
                                     DemodStats* stats = nullptr) {
  Demodulator d(cfg);
  const std::size_t n = iq.size() / 2;
  if (chunk == 0) chunk = cfg.block_samples;
  std::vector<DemodFrame> out;
  for (std::size_t off = 0; off < n; off += chunk) {
    const auto got = d.feed(reinterpret_cast<const std::uint8_t*>(iq.data() + 2 * off), std::min(chunk, n - off),
                            SampleFormat::kCf32);
    out.insert(out.end(), got.begin(), got.end());
  }
  const auto rest = d.flush();
  out.insert(out.end(), rest.begin(), rest.end());
  if (stats) *stats = d.stats();
  return out;
}

std::vector<DemodFrame> mode_ac_only(const std::vector<DemodFrame>& f) {
  std::vector<DemodFrame> out;
  for (const auto& x : f) {
    if (x.len == 2) out.push_back(x);
  }
  return out;
}

std::uint16_t code_of(const DemodFrame& f) { return static_cast<std::uint16_t>(f.bytes[0] << 8 | f.bytes[1]); }

// Noise sigma per component for a pulse amplitude and an SNR (A^2 / 2 sigma^2).
double sigma_for(double amp, double snr_db) { return amp / std::sqrt(2.0 * std::pow(10.0, snr_db / 10.0)); }

// ---------------------------------------------------------------------------
// Demodulator
// ---------------------------------------------------------------------------

TEST(ModeAcDemod, OffByDefaultAndValidated) {
  EXPECT_FALSE(DemodConfig{}.mode_ac);
  DemodConfig c = ac_config(2.4e6);
  std::string why;
  EXPECT_TRUE(validate(c, &why)) << why;
  c.ac_decision_band = 0.5;
  EXPECT_FALSE(validate(c, &why));
  c = ac_config(2.4e6);
  c.ac_min_signal_ratio = 0.5;
  EXPECT_FALSE(validate(c, &why));
  c = ac_config(2.4e6);
  c.ac_max_pulse = 1.0;
  EXPECT_FALSE(validate(c, &why));
  c = ac_config(2.4e6);
  c.ac_framing_ratio = 0.9;
  EXPECT_FALSE(validate(c, &why));
  // Off: a reply alone gives nothing.
  const double fs = 2.4e6;
  const auto iq = render({{reply_pulses(0x1200), 400.0, 0.3}}, fs, 20000, sigma_for(0.3, 30));
  DemodConfig off;
  off.sample_rate_hz = fs;
  EXPECT_TRUE(demod_floats(iq, off).empty());
}

TEST(ModeAcDemod, DecodesASingleReplyAtEveryRateAndOffset) {
  const std::uint16_t codes[] = {0x7700, 0x1200, 0x1280, 0x7777, 0x0000, *feet_to_gillham(37000), *feet_to_gillham(-300)};
  for (const double fs : {2.0e6, 2.4e6, 6e6, 8e6}) {
    int n = 0, hits = 0, wrong = 0;
    for (const double snr : {18.0, 24.0, 30.0}) {
      int seed = 1;
      for (const std::uint16_t code : codes) {
        for (const double frac : {0.0, 0.3, 0.55, 0.8}) {
          const double start_us = 300.0 + frac / (fs / 1e6);
          const auto iq = render({{reply_pulses(code), start_us, 0.3, 1.1 * seed, 12e3 * seed}}, fs,
                                 static_cast<std::size_t>(fs * 0.004), sigma_for(0.3, snr), static_cast<std::uint64_t>(seed));
          ++seed;
          const auto got = demod_floats(iq, ac_config(fs));
          ++n;
          ASSERT_LE(got.size(), 1u) << fs << " " << snr << " " << std::hex << code;
          if (got.empty()) {
            EXPECT_LT(snr, 30.0) << "missed at 30 dB: " << fs << " " << std::hex << code;
            continue;
          }
          const DemodFrame& f = got[0];
          EXPECT_EQ(f.len, 2);
          EXPECT_EQ(f.df, 32);
          if (snr >= 30.0) {
            EXPECT_EQ(code_of(f), code) << fs << " " << snr;
          }
          if (code_of(f) != code) ++wrong;
          const double truth = start_us * fs / 1e6;
          EXPECT_LT(std::fabs(f.sample_pos - truth), 0.5) << fs << " " << snr << " frac " << frac;
          EXPECT_NEAR(f.signal, 0.3f, 0.06f);
          if (code_of(f) == code) ++hits;
        }
      }
    }
    // 18 dB is a third of the cases; at 2.0 MS/s a pulse (0.9 samples) can
    // fall across two samples and lose half its level.
    EXPECT_GE(hits, fs < 2.2e6 ? n * 85 / 100 : n * 95 / 100) << fs;
    EXPECT_LE(wrong, n / 40) << fs;
  }
}

TEST(ModeAcDemod, SpiIsAPulseAfterF2AndXMustBeEmpty) {
  const double fs = 2.4e6;
  const double sigma = sigma_for(0.3, 30);
  {
    const auto got = demod_floats(render({{reply_pulses(0x4521 | 0x0080), 200.0}}, fs, 5000, sigma), ac_config(fs));
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(code_of(got[0]), 0x45A1);
  }
  {
    const auto got = demod_floats(render({{reply_pulses(0x4521), 200.0}}, fs, 5000, sigma), ac_config(fs));
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(code_of(got[0]), 0x4521);
  }
  // A pulse in X, or in an always-empty slot after F2: not a reply.
  for (const int slot : {7, 15, 16, 18, 19}) {
    DemodStats st;
    const auto got =
        demod_floats(render({{reply_pulses(0x4521, 1u << slot), 200.0}}, fs, 5000, sigma), ac_config(fs), 0, &st);
    EXPECT_TRUE(got.empty()) << slot;
    if (slot == 7 || slot == 15) {
      EXPECT_EQ(st.ac_framing, 0u) << slot << ": the gate already checks X and slot 15";
    } else {
      EXPECT_GT(st.ac_garbled, 0u) << slot;
    }
  }
}

TEST(ModeAcDemod, OverlappingRepliesAreRejectedNotMisread) {
  // Two replies whose pulses interleave (offsets that are not a multiple of
  // 1.45 us) put pulses in each other's gaps. Of similar strength, both are
  // dropped; a much weaker one may leave the stronger readable, but nothing
  // is ever read with a code or a start that neither reply has.
  const double fs = 2.4e6;
  int cases = 0, decoded = 0;
  for (const double offset_us : {0.6, 2.0, 3.3, 5.1, 9.95, 17.0}) {
    for (const double ratio : {1.0, 0.7, 0.5, 0.25}) {
      DemodStats st;
      const double t1 = 300.0, t2 = 300.0 + offset_us;
      const auto got = demod_floats(
          render({{reply_pulses(0x1200), t1, 0.3, 0.0, 5e3}, {reply_pulses(0x6543), t2, 0.3 * ratio, 1.0, -9e3}},
                 fs, 8000, sigma_for(0.3, 30), static_cast<std::uint64_t>(++cases)),
          ac_config(fs), 0, &st);
      for (const auto& f : got) {
        ++decoded;
        const bool first = code_of(f) == 0x1200 && std::fabs(f.sample_pos - t1 * 2.4) < 1.0;
        const bool second = code_of(f) == 0x6543 && std::fabs(f.sample_pos - t2 * 2.4) < 1.0;
        EXPECT_TRUE(first || second) << "offset " << offset_us << " ratio " << ratio << ": decoded " << std::hex << code_of(f) << " at " << f.sample_pos / 2.4;
      }
      if (ratio > 0.6) {
        EXPECT_TRUE(got.empty()) << "offset " << offset_us << " ratio " << ratio;
        EXPECT_GT(st.ac_garbled, 0u) << offset_us << " " << ratio;
      }
    }
  }
  EXPECT_LT(decoded, cases);
  // Far enough apart, both decode.
  const auto got = demod_floats(
      render({{reply_pulses(0x1200), 300.0, 0.3}, {reply_pulses(0x6543), 300.0 + 27.0, 0.2, 1.0}}, fs, 8000, sigma_for(0.3, 30)),
      ac_config(fs));
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(code_of(got[0]), 0x1200);
  EXPECT_EQ(code_of(got[1]), 0x6543);
}

TEST(ModeAcDemod, AReplyUnderADecodedModeSFrameIsSkipped) {
  const double fs = 2.4e6;
  const auto df17 = adsb::modes::encode_airborne_position(0xABCDEF, 30000, adsb::modes::CprFrame{1000, 2000, false});
  for (const double at_us : {-10.0, 20.0, 60.0, 115.0}) {
    DemodStats st;
    const auto got = demod_floats(
        render({{mode_s_pulses(df17), 300.0, 0.4}, {reply_pulses(0x1200), 300.0 + at_us, 0.1, 1.0}}, fs, 12000,
               sigma_for(0.4, 35)),
        ac_config(fs), 0, &st);
    ASSERT_EQ(got.size(), 1u) << at_us;
    EXPECT_EQ(got[0].len, 14);
    EXPECT_GT(st.ac_in_mode_s, 0u) << at_us;
  }
  // Clear of it (the frame ends at 420 us), the reply is found.
  const auto got = demod_floats(
      render({{mode_s_pulses(df17), 300.0, 0.4}, {reply_pulses(0x1200), 300.0 + 125.0, 0.1, 1.0}}, fs, 12000, sigma_for(0.4, 35)),
      ac_config(fs));
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].len, 14);
  EXPECT_EQ(got[1].len, 2);
  EXPECT_EQ(code_of(got[1]), 0x1200);
}

TEST(ModeAcDemod, AReplyJustBeforeAModeSFrameComesOutFirstWhereverAFeedEnds) {
  // The reply is decided only once the Mode S scan is a reply's length past
  // it, so the Mode S frame after it is held back until then: for every
  // split of the input, both are found and the reply comes out first.
  // (Emitting the frame at once would also hide the reply: it would count
  // as already past when the reply is scanned.)
  const double fs = 2.4e6;
  const auto df17 = adsb::modes::encode_airborne_position(0xABCDEF, 30000, adsb::modes::CprFrame{1000, 2000, false});
  const auto iq = render({{reply_pulses(0x1200), 1000.0, 0.2}, {mode_s_pulses(df17), 1000.0 + 29.0, 0.3, 1.0}}, fs, 6000,
                         sigma_for(0.3, 35));
  int both = 0;
  for (std::size_t first = 2400; first < 3200; ++first) {
    DemodConfig cfg = ac_config(fs);
    cfg.block_samples = 4096;
    Demodulator d(cfg);
    std::vector<DemodFrame> out;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(iq.data());
    auto a = d.feed(bytes, first, SampleFormat::kCf32);
    out.insert(out.end(), a.begin(), a.end());
    a = d.feed(bytes + first * 8, iq.size() / 2 - first, SampleFormat::kCf32);
    out.insert(out.end(), a.begin(), a.end());
    a = d.flush();
    out.insert(out.end(), a.begin(), a.end());
    if (out.size() == 2) ++both;
    for (std::size_t i = 1; i < out.size(); ++i) {
      ASSERT_LE(out[i - 1].sample_pos, out[i].sample_pos) << "first feed " << first;
    }
  }
  EXPECT_EQ(both, 800) << "every split finds both";
}

TEST(ModeAcDemod, PureNoiseGivesAlmostNoDetections) {
  // Three seconds of Gaussian noise at 2.4 MS/s and one at 8 MS/s. At the default
  // thresholds a few false detections per minute are expected (see
  // core/README.md); this bounds the rate at well under one per second.
  for (const double fs : {2.4e6, 8e6}) {
    const double seconds = fs < 3e6 ? 3.0 : 1.0;
    DemodStats st;
    const auto got = demod_floats(render({}, fs, static_cast<std::size_t>(fs * seconds), 0.01, 99), ac_config(fs), 0, &st);
    EXPECT_LE(mode_ac_only(got).size(), 2u) << fs;
    EXPECT_GT(st.ac_gated, 0u);
  }
}

TEST(ModeAcDemod, ToBeastWritesType1StampedAtF2) {
  DemodFrame f;
  f.len = 2;
  f.df = 32;
  f.bytes[0] = 0x1A;  // escaped in the stream
  f.bytes[1] = 0x80;
  f.sample_pos = 1000.0;
  f.signal = 0.5f;
  std::vector<std::uint8_t> out;
  to_beast(f, 2.4e6, out);
  adsb::ingest::BeastParser p;
  int n = 0;
  p.feed(out.data(), out.size(), [&](const adsb::ingest::BeastFrame& bf) {
    ++n;
    EXPECT_EQ(bf.type, adsb::ingest::BeastType::kModeAC);
    EXPECT_EQ(bf.len, 2);
    EXPECT_EQ(bf.payload[0], 0x1A);
    EXPECT_EQ(bf.payload[1], 0x80);
    // F2 is 20.3 us = 48.72 samples after F1; 5 ticks per sample.
    EXPECT_EQ(bf.timestamp, static_cast<std::uint64_t>(std::llround((1000.0 + 48.72) * 5.0)));
  });
  EXPECT_EQ(n, 1);
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

adsb::sim::SynthConfig synth_config(int aircraft, double seconds, std::uint64_t seed) {
  adsb::sim::SynthConfig syn;
  syn.seed = seed;
  syn.aircraft = aircraft;
  syn.duration_s = seconds;
  syn.keep_transmissions = true;
  return syn;
}

adsb::sim::IqConfig iq_config(std::uint64_t seed, double modeac_hz) {
  adsb::sim::IqConfig cfg;
  cfg.seed = seed;
  cfg.modeac_rate_hz = modeac_hz;
  cfg.modeac_spi_share = 0.2;
  return cfg;
}

bool same_mode_s(const adsb::sim::IqMessage& a, const adsb::sim::IqMessage& b) {
  return a.tx_ns == b.tx_ns && a.icao == b.icao && a.len == b.len && std::memcmp(a.bytes, b.bytes, a.len) == 0 &&
         a.sample_pos == b.sample_pos && a.first_sample == b.first_sample && a.last_sample == b.last_sample &&
         a.snr_db == b.snr_db && a.tx_offset_hz == b.tx_offset_hz && a.phase0 == b.phase0 && a.observed_hz == b.observed_hz;
}

TEST(IqModeAc, ModeSMessagesAreUnchangedByModeAcReplies) {
  const auto syn = synth_config(40, 3.0, 61);
  const auto traffic = adsb::sim::generate(syn);
  const auto off = adsb::sim::plan_iq(syn, traffic, iq_config(61, 0.0));
  const auto on = adsb::sim::plan_iq(syn, traffic, iq_config(61, 10.0));
  std::vector<const adsb::sim::IqMessage*> s_on;
  for (const auto& m : on.messages) {
    if (!m.mode_ac) s_on.push_back(&m);
  }
  ASSERT_EQ(s_on.size(), off.messages.size());
  for (std::size_t i = 0; i < s_on.size(); ++i) {
    ASSERT_TRUE(same_mode_s(*s_on[i], off.messages[i])) << i;
  }
  EXPECT_GT(on.stats.modeac_replies, 0u);
  EXPECT_EQ(on.messages.size(), off.messages.size() + on.stats.modeac_replies);
  EXPECT_EQ(off.stats.modeac_replies, 0u);
  // The metadata of a Mode S-only plan has no Mode A/C fields.
  const std::string meta_off = adsb::sim::sigmf_meta(syn, iq_config(61, 0.0), off);
  EXPECT_EQ(meta_off.find("mode_ac"), std::string::npos);
  EXPECT_EQ(meta_off.find("MODE"), std::string::npos);
  const std::string meta_on = adsb::sim::sigmf_meta(syn, iq_config(61, 10.0), on);
  EXPECT_NE(meta_on.find("\"adsb:mode_ac\": {\"rate_hz\": 10.0000"), std::string::npos);
  EXPECT_NE(meta_on.find("\"core:label\": \"MODEA\""), std::string::npos);
  EXPECT_NE(meta_on.find("\"core:label\": \"MODEC\""), std::string::npos);
  // The samples are the same wherever no Mode A/C reply reaches.
  std::vector<float> a(2 * adsb::sim::kIqChunkSamples), b(2 * adsb::sim::kIqChunkSamples);
  std::vector<bool> touched(static_cast<std::size_t>(on.total_samples), false);
  for (const auto& m : on.messages) {
    if (!m.mode_ac) continue;
    for (std::int64_t k = m.first_sample; k <= m.last_sample && k < static_cast<std::int64_t>(touched.size()); ++k) {
      touched[static_cast<std::size_t>(k)] = true;
    }
  }
  std::size_t compared = 0;
  for (std::uint64_t c = 0; c < 4; ++c) {
    const std::size_t na = adsb::sim::render_iq_chunk(off, iq_config(61, 0.0), c, a.data());
    const std::size_t nb = adsb::sim::render_iq_chunk(on, iq_config(61, 10.0), c, b.data());
    ASSERT_EQ(na, nb);
    for (std::size_t k = 0; k < na; ++k) {
      if (touched[c * adsb::sim::kIqChunkSamples + k]) continue;
      ++compared;
      if (a[2 * k] != b[2 * k] || a[2 * k + 1] != b[2 * k + 1]) {
        FAIL() << "sample " << c * adsb::sim::kIqChunkSamples + k << " differs";
      }
    }
  }
  EXPECT_GT(compared, 100000u);
}

TEST(IqModeAc, RepliesCarryTheSquawkOrTheAltitudeAndComeAtTheConfiguredRate) {
  const double rate = 8.0, seconds = 20.0;
  const auto syn = synth_config(50, seconds, 62);
  const auto traffic = adsb::sim::generate(syn);
  adsb::sim::IqConfig cfg = iq_config(62, rate);
  cfg.min_snr_db = -999;  // keep every reply
  const auto plan = adsb::sim::plan_iq(syn, traffic, cfg);
  std::map<std::uint32_t, const adsb::sim::AircraftInfo*> info;
  for (const auto& a : traffic.aircraft) info[a.icao] = &a;
  std::size_t airborne = 0;
  for (const auto& a : traffic.aircraft) airborne += a.on_ground ? 0u : 1u;
  std::size_t n_a = 0, n_c = 0;
  std::map<std::uint32_t, std::set<bool>> spi_of;
  for (const auto& m : plan.messages) {
    if (!m.mode_ac) continue;
    ASSERT_EQ(m.len, 2);
    const auto code = static_cast<std::uint16_t>(m.bytes[0] << 8 | m.bytes[1]);
    const auto* a = info.at(m.icao);
    EXPECT_FALSE(a->on_ground) << "no Mode A/C from the ground";
    if (m.mode_ac == 'A') {
      ++n_a;
      EXPECT_EQ(code & 0x7777u, a->squawk);
      spi_of[m.icao].insert((code & 0x0080u) != 0);
    } else {
      ASSERT_EQ(m.mode_ac, 'C');
      ++n_c;
      EXPECT_EQ(code & 0x0080u, 0u) << "SPI only in Mode A replies";
      const auto ft = gillham_to_feet(code);
      ASSERT_TRUE(ft.has_value());
      EXPECT_LE(std::fabs(*ft - m.alt_ft), 50.0);
    }
    EXPECT_DOUBLE_EQ(m.sample_pos, m.arrival_s * cfg.sample_rate_hz);
  }
  const double expected = rate * seconds * static_cast<double>(airborne);
  EXPECT_NEAR(static_cast<double>(n_a + n_c), expected, 0.25 * expected);
  EXPECT_NEAR(static_cast<double>(n_a), static_cast<double>(n_c), 0.15 * static_cast<double>(n_a + n_c));
  // An aircraft's ident is on or off for the whole run, and about 20% have it.
  std::size_t with_spi = 0;
  for (const auto& [icao, s] : spi_of) {
    EXPECT_EQ(s.size(), 1u);
    with_spi += *s.begin() ? 1u : 0u;
  }
  EXPECT_GT(with_spi, 0u);
  EXPECT_LT(with_spi, spi_of.size() / 2);
}

TEST(IqModeAc, TheWaveformHasTheReplysPulses) {
  adsb::sim::SynthConfig syn;
  syn.duration_s = 0.002;
  adsb::sim::Transmission tr;
  tr.tx_ns = syn.start_rx_ns + 200000;
  tr.icao = 0xABCDEF;
  tr.lat = syn.receiver_lat + 0.5;
  tr.lon = syn.receiver_lon;
  tr.alt_ft = 20000;
  tr.len = 7;
  const auto f = adsb::modes::encode_df11(0xABCDEF, 5);
  std::memcpy(tr.bytes, f.bytes, 7);
  adsb::sim::SynthOutput traffic;
  adsb::sim::AircraftInfo info;
  info.icao = tr.icao;
  info.squawk = 0x7700;
  traffic.aircraft.push_back(info);
  traffic.transmissions.push_back(tr);
  adsb::sim::IqConfig cfg;
  cfg.noise = false;
  cfg.ground_reflection = 0;
  cfg.antenna = adsb::sim::AntennaPattern::kOmni;
  cfg.full_scale_dbm = -40;
  cfg.sample_rate_hz = 20e6;
  cfg.modeac_rate_hz = 1000;  // a burst within the 2 ms
  cfg.modeac_radars = 1;
  cfg.modeac_beam_deg = 360;  // always in the beam
  cfg.modeac_mode_c_share = 0.0;
  const auto plan = adsb::sim::plan_iq(syn, traffic, cfg);
  std::vector<float> iq(2 * plan.total_samples);
  std::vector<float> chunk(2 * adsb::sim::kIqChunkSamples);
  for (std::uint64_t c = 0;; ++c) {
    const std::size_t n = adsb::sim::render_iq_chunk(plan, cfg, c, chunk.data());
    if (n == 0) break;
    std::memcpy(iq.data() + 2 * c * adsb::sim::kIqChunkSamples, chunk.data(), 2 * n * sizeof(float));
  }
  int checked = 0;
  for (const auto& m : plan.messages) {
    if (m.mode_ac != 'A' || m.collided) continue;
    const double amp = std::sqrt(adsb::pm::db_to_power(m.link.power_dbm - cfg.full_scale_dbm));
    const std::uint32_t pulses = mode_ac_pulses(0x7700);
    for (int k = 0; k <= 14; ++k) {  // to F2 (the recording may end soon after it)
      const auto s = static_cast<std::size_t>(std::llround(m.sample_pos + (1.45 * k + 0.225) * 20.0));
      const double mag = std::hypot(static_cast<double>(iq[2 * s]), static_cast<double>(iq[2 * s + 1]));
      if (pulses >> k & 1u) {
        EXPECT_NEAR(mag, amp, 0.02 * amp) << k;
      } else {
        EXPECT_LT(mag, 1e-6) << k;
      }
    }
    ++checked;
  }
  EXPECT_GT(checked, 0);
}

// ---------------------------------------------------------------------------
// Scenarios: the generator's replies through the demodulator
// ---------------------------------------------------------------------------

struct Scenario {
  adsb::sim::IqPlan plan;
  std::vector<std::uint8_t> cu8;
};

Scenario scenario(int aircraft, double seconds, std::uint64_t seed, double modeac_hz) {
  const auto syn = synth_config(aircraft, seconds, seed);
  const auto traffic = adsb::sim::generate(syn);
  const auto cfg = iq_config(seed, modeac_hz);
  Scenario s;
  s.plan = adsb::sim::plan_iq(syn, traffic, cfg);
  s.cu8.resize(s.plan.total_samples * 2);
  std::vector<float> chunk(2 * adsb::sim::kIqChunkSamples);
  for (std::uint64_t c = 0; c * adsb::sim::kIqChunkSamples < s.plan.total_samples; ++c) {
    const std::size_t n = adsb::sim::render_iq_chunk(s.plan, cfg, c, chunk.data());
    adsb::sim::quantize_iq(chunk.data(), n, adsb::sim::IqFormat::kCu8, s.cu8.data() + c * adsb::sim::kIqChunkSamples * 2);
  }
  return s;
}

std::vector<DemodFrame> demod_cu8(const std::vector<std::uint8_t>& data, const DemodConfig& cfg, std::size_t chunk = 0,
                                  DemodStats* stats = nullptr) {
  Demodulator d(cfg);
  const std::size_t n = data.size() / 2;
  if (chunk == 0) chunk = cfg.block_samples;
  std::vector<DemodFrame> out;
  for (std::size_t off = 0; off < n; off += chunk) {
    const auto got = d.feed(data.data() + off * 2, std::min(chunk, n - off), SampleFormat::kCu8);
    out.insert(out.end(), got.begin(), got.end());
  }
  const auto rest = d.flush();
  out.insert(out.end(), rest.begin(), rest.end());
  if (stats) *stats = d.stats();
  return out;
}

struct AcScore {
  std::size_t clean = 0, clean_hit = 0, matched = 0, wrong = 0, false_frames = 0;
};

// A decoded reply matches a truth reply with its code within 2 samples.
AcScore score(const adsb::sim::IqPlan& plan, const std::vector<DemodFrame>& frames) {
  AcScore s;
  std::vector<bool> used(plan.messages.size(), false);
  for (const DemodFrame& f : frames) {
    if (f.len != 2) continue;
    std::size_t best = plan.messages.size(), near = plan.messages.size();
    for (std::size_t i = 0; i < plan.messages.size(); ++i) {
      const auto& m = plan.messages[i];
      if (!m.mode_ac || used[i] || std::fabs(m.sample_pos - f.sample_pos) > 2.0) continue;
      if (m.bytes[0] == f.bytes[0] && m.bytes[1] == f.bytes[1]) best = i;
      near = i;
    }
    if (best < plan.messages.size()) {
      used[best] = true;
      ++s.matched;
    } else if (near < plan.messages.size()) {
      ++s.wrong;
    } else {
      ++s.false_frames;
    }
  }
  for (std::size_t i = 0; i < plan.messages.size(); ++i) {
    const auto& m = plan.messages[i];
    if (m.mode_ac && !m.collided && m.snr_db >= 15 && m.snr_db < 30) {
      ++s.clean;
      if (used[i]) ++s.clean_hit;
    }
  }
  return s;
}

TEST(ModeAcScenario, DecodesTheGeneratorsReplies) {
  const Scenario sc = scenario(60, 3.0, 63, 10.0);
  DemodConfig cfg;
  cfg.mode_ac = true;
  const auto frames = demod_cu8(sc.cu8, cfg);
  const AcScore s = score(sc.plan, frames);
  ASSERT_GT(s.clean, 800u);
  EXPECT_GE(static_cast<double>(s.clean_hit) / static_cast<double>(s.clean), 0.95);
  EXPECT_LE(static_cast<double>(s.wrong), 0.01 * static_cast<double>(s.matched));
  EXPECT_LE(s.false_frames, 5u);
  for (std::size_t i = 1; i < frames.size(); ++i) {
    EXPECT_LE(frames[i - 1].sample_pos, frames[i].sample_pos) << "frames in time order";
  }
}

TEST(ModeAcScenario, ModeSFramesAreExactlyThoseOfAModeSOnlyDemodulator) {
  const Scenario sc = scenario(60, 2.0, 64, 10.0);
  DemodConfig off, on;
  on.mode_ac = true;
  DemodStats st_off, st_on;
  const auto a = demod_cu8(sc.cu8, off, 0, &st_off);
  const auto b_all = demod_cu8(sc.cu8, on, 0, &st_on);
  std::vector<DemodFrame> b;
  for (const auto& f : b_all) {
    if (f.len != 2) b.push_back(f);
  }
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GT(b_all.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::memcmp(a[i].bytes, b[i].bytes, 14), 0) << i;
    EXPECT_EQ(a[i].sample_pos, b[i].sample_pos) << i;
    EXPECT_EQ(a[i].signal, b[i].signal) << i;
    EXPECT_EQ(a[i].noise, b[i].noise) << i;
    EXPECT_EQ(a[i].margin, b[i].margin) << i;
  }
  EXPECT_EQ(st_off.gated, st_on.gated);
  EXPECT_EQ(st_off.preambles, st_on.preambles);
  EXPECT_EQ(st_off.frames, st_on.frames);
  EXPECT_EQ(st_off.ac_frames, 0u);
  EXPECT_EQ(st_on.ac_frames, b_all.size() - b.size());
}

TEST(ModeAcScenario, OutputDoesNotDependOnHowTheInputIsSplit) {
  const Scenario sc = scenario(40, 1.0, 65, 10.0);
  DemodConfig cfg;
  cfg.mode_ac = true;
  const auto whole = demod_cu8(sc.cu8, cfg);
  for (const std::size_t chunk : {std::size_t{1024}, std::size_t{7777}, std::size_t{30001}}) {
    // Small blocks too (each feed() returns at most block_samples' worth):
    // replies decided later than the Mode S frames after them must still
    // come out in time order.
    DemodConfig small = cfg;
    small.block_samples = std::max<std::size_t>(chunk, 1024);
    const auto parts = demod_cu8(sc.cu8, small, chunk);
    for (std::size_t i = 1; i < parts.size(); ++i) {
      ASSERT_LE(parts[i - 1].sample_pos, parts[i].sample_pos) << "time order, chunk " << chunk << " frame " << i;
    }
    ASSERT_EQ(parts.size(), whole.size()) << chunk;
    for (std::size_t i = 0; i < whole.size(); ++i) {
      EXPECT_EQ(parts[i].len, whole[i].len);
      EXPECT_EQ(std::memcmp(parts[i].bytes, whole[i].bytes, 14), 0);
      EXPECT_EQ(parts[i].sample_pos, whole[i].sample_pos) << chunk << " " << i;
    }
  }
}

// Every bit of every field of every frame (%a), Mode S and Mode A/C, and the
// counts of the Mode A/C decisions: equal on every platform.
std::string pin_of(const std::vector<DemodFrame>& frames, const DemodStats& st) {
  std::string all;
  for (const DemodFrame& f : frames) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d %02X%02X%02X %a %a %a %a\n", f.len, f.bytes[0], f.bytes[1], f.bytes[2], f.sample_pos,
                  static_cast<double>(f.signal), static_cast<double>(f.margin), static_cast<double>(f.noise));
    all += buf;
  }
  all += std::to_string(st.ac_gated) + " " + std::to_string(st.ac_in_mode_s) + " " + std::to_string(st.ac_framing) + " " +
         std::to_string(st.ac_garbled) + " " + std::to_string(st.gated) + " " + std::to_string(st.preambles);
  adsb::Sha256 sha;
  sha.update(all.data(), all.size());
  return adsb::Sha256::hex(sha.finish()).substr(0, 16) + " " + std::to_string(st.ac_frames) + " " +
         std::to_string(frames.size());
}

TEST(ModeAcScenario, OutputIsPinned) {
  const Scenario sc = scenario(40, 1.0, 66, 10.0);
  DemodConfig cfg;
  cfg.mode_ac = true;
  DemodStats st;
  const auto frames = demod_cu8(sc.cu8, cfg, 0, &st);
  EXPECT_EQ(pin_of(frames, st), "67222bd88b43c143 247 429");
}

// ---------------------------------------------------------------------------
// Downstream: a 2-byte frame
// ---------------------------------------------------------------------------

struct NullSink {
  std::size_t updates = 0;
  void on_update(const adsb::protocol::TrackUpdate&) { ++updates; }
  void on_delete(const adsb::protocol::TrackDelete&) {}
};

TEST(ModeAcDownstream, TheTrackEngineCountsModeAcAndTracksNothing) {
  adsb::state::TrackEngine engine;
  NullSink sink;
  const std::uint8_t code[2] = {0x77, 0x00};
  adsb::state::FrameMeta meta;
  meta.rx_ns = 1'000'000'000;
  engine.on_frame(code, 2, meta, sink);
  EXPECT_EQ(engine.stats().frames, 1u);
  EXPECT_EQ(engine.stats().mode_ac, 1u);
  EXPECT_EQ(engine.stats().bad_frames, 0u);
  EXPECT_EQ(sink.updates, 0u);
  adsb::state::NaiveTrackEngine naive;
  naive.on_frame(code, 2, meta, sink);
  EXPECT_EQ(naive.stats().mode_ac, 1u);
}

TEST(ModeAcDownstream, TheArbiterPassesModeAcThrough) {
  adsb::ingest::Arbiter arb;
  adsb::record::Record r;
  r.type = '1';
  r.len = 2;
  r.payload[0] = 0x12;
  r.rx_ns = 5;
  EXPECT_TRUE(arb.offer(r));
  r.line = 1;
  EXPECT_TRUE(arb.offer(r)) << "the same code on line B is not a copy";
  EXPECT_EQ(arb.stats().unarbitrated, 2u);
}

TEST(ModeAcDownstream, IqSourceRecordsModeAcAsBeastType1) {
  const Scenario sc = scenario(30, 1.0, 67, 10.0);
  char path[] = "/tmp/adsb_mode_ac_XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(write(fd, sc.cu8.data(), sc.cu8.size()), static_cast<ssize_t>(sc.cu8.size()));
  close(fd);
  adsb::ingest::IqSourceConfig cfg;
  cfg.path = path;
  cfg.speed = 0;
  cfg.demod.mode_ac = true;
  adsb::ingest::IqSource src(cfg);
  std::string err;
  ASSERT_TRUE(src.open(&err)) << err;
  std::vector<adsb::record::Record> recs;
  std::atomic<bool> stop{false};
  src.run(stop, [&](const adsb::record::Record& r) { recs.push_back(r); });
  std::remove(path);
  DemodConfig dc;
  dc.mode_ac = true;
  dc.block_samples = cfg.block_samples;
  const auto ref = demod_cu8(sc.cu8, dc);
  ASSERT_EQ(recs.size(), ref.size());
  std::size_t ac = 0;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (ref[i].len != 2) continue;
    ++ac;
    EXPECT_EQ(recs[i].type, '1');
    EXPECT_EQ(recs[i].len, 2);
    EXPECT_EQ(recs[i].payload[0], ref[i].bytes[0]);
    EXPECT_EQ(recs[i].payload[1], ref[i].bytes[1]);
    EXPECT_EQ(recs[i].radio_ts, static_cast<std::uint64_t>(std::llround((ref[i].sample_pos + 20.3 * 2.4) * 5.0)));
  }
  EXPECT_GT(ac, 100u);
}

}  // namespace
