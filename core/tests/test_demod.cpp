// Tests for the Mode S demodulator (demod/demod.hpp).
//
// Most tests render their own IQ with an ideal PPM model written here
// (rectangular pulses, exact sample overlap, a carrier, Gaussian noise), so
// they do not depend on the scenario generator; the scenario tests at the end
// run the demodulator on generator output and score it against its truth.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "adsb/common/hash.hpp"
#include "adsb/common/portable_math.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/ingest/beast.hpp"
#include "adsb/modes/crc24.hpp"
#include "adsb/modes/encoder.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb::demod;
using adsb::modes::Frame;

// ---------------------------------------------------------------------------
// An independent PPM renderer
// ---------------------------------------------------------------------------

struct Burst {
  Frame frame;
  double start_us = 0.0;   // preamble start
  double amp = 0.3;        // relative to full scale
  double phase = 0.0;
  double freq_hz = 0.0;    // carrier offset
};

// Interleaved I/Q floats: sample k covers [(k - 0.5) / fs, (k + 0.5) / fs).
std::vector<float> render(const std::vector<Burst>& bursts, double fs, std::size_t n, double noise_sigma,
                          std::uint64_t seed = 1) {
  std::vector<double> iq(2 * n, 0.0);
  const double us = fs / 1e6;  // samples per microsecond
  for (const Burst& b : bursts) {
    std::vector<std::pair<double, double>> pulses = {{0, 0.5}, {1, 1.5}, {3.5, 4}, {4.5, 5}};
    for (int i = 0; i < b.frame.len * 8; ++i) {
      const bool one = (b.frame.bytes[i / 8] >> (7 - i % 8)) & 1;
      const double a = 8.0 + i + (one ? 0.0 : 0.5);
      pulses.emplace_back(a, a + 0.5);
    }
    for (auto [pa, pb] : pulses) {
      const double xa = (b.start_us + pa) * us;  // sample units
      const double xb = (b.start_us + pb) * us;
      const auto k0 = static_cast<std::ptrdiff_t>(std::floor(xa + 0.5));
      const auto k1 = static_cast<std::ptrdiff_t>(std::floor(xb + 0.5));
      for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(k0, 0); k <= k1 && k < static_cast<std::ptrdiff_t>(n); ++k) {
        const double lo = std::max(xa, static_cast<double>(k) - 0.5);
        const double hi = std::min(xb, static_cast<double>(k) + 0.5);
        if (hi <= lo) continue;
        const double ph = b.phase + 2 * M_PI * b.freq_hz * static_cast<double>(k) / fs;
        iq[2 * static_cast<std::size_t>(k)] += b.amp * (hi - lo) * std::cos(ph);
        iq[2 * static_cast<std::size_t>(k) + 1] += b.amp * (hi - lo) * std::sin(ph);
      }
    }
  }
  adsb::Rng rng(seed);
  std::vector<float> out(2 * n);
  for (std::size_t i = 0; i < 2 * n; i += 2) {
    double g0 = 0, g1 = 0;
    if (noise_sigma > 0) {  // Box-Muller
      const double u1 = 1.0 - rng.uniform();
      const double u2 = rng.uniform();
      const double r = std::sqrt(-2.0 * adsb::pm::log(u1)) * noise_sigma;
      double s, c;
      adsb::pm::sincos(2 * M_PI * u2, s, c);
      g0 = r * c;
      g1 = r * s;
    }
    out[i] = static_cast<float>(iq[i] + g0);
    out[i + 1] = static_cast<float>(iq[i + 1] + g1);
  }
  return out;
}

std::vector<std::uint8_t> as_bytes(const std::vector<float>& iq, SampleFormat f) {
  const std::size_t n = iq.size() / 2;
  std::vector<std::uint8_t> out(n * bytes_per_sample(f));
  for (std::size_t i = 0; i < iq.size(); ++i) {
    const double v = std::clamp(static_cast<double>(iq[i]), -1.0, 1.0);
    switch (f) {
      case SampleFormat::kCu8: out[i] = static_cast<std::uint8_t>(std::lround(v * 127.5 + 127.5)); break;
      case SampleFormat::kCs8: out[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(v * 127.0))); break;
      case SampleFormat::kCs16: {
        const auto s = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 32767.0)));
        out[2 * i] = static_cast<std::uint8_t>(s & 0xFF);
        out[2 * i + 1] = static_cast<std::uint8_t>(s >> 8);
        break;
      }
      case SampleFormat::kCf32: std::memcpy(out.data() + 4 * i, &iq[i], 4); break;
    }
  }
  return out;
}

// Runs a whole buffer through a demodulator in `chunk`-sample calls.
std::vector<DemodFrame> demod_all(const std::vector<std::uint8_t>& data, SampleFormat f, const DemodConfig& cfg,
                                  std::size_t chunk = 0, DemodStats* stats = nullptr) {
  Demodulator d(cfg);
  const std::size_t bps = bytes_per_sample(f);
  const std::size_t n = data.size() / bps;
  if (chunk == 0) chunk = cfg.block_samples;
  std::vector<DemodFrame> out;
  for (std::size_t off = 0; off < n; off += chunk) {
    const auto got = d.feed(data.data() + off * bps, std::min(chunk, n - off), f);
    out.insert(out.end(), got.begin(), got.end());
  }
  const auto rest = d.flush();
  out.insert(out.end(), rest.begin(), rest.end());
  if (stats) *stats = d.stats();
  return out;
}

std::vector<DemodFrame> demod_floats(const std::vector<float>& iq, const DemodConfig& cfg, DemodStats* stats = nullptr) {
  return demod_all(as_bytes(iq, SampleFormat::kCf32), SampleFormat::kCf32, cfg, 0, stats);
}

std::string hex(const std::uint8_t* b, int len) {
  std::string s;
  char buf[3];
  for (int i = 0; i < len; ++i) {
    std::snprintf(buf, sizeof buf, "%02X", b[i]);
    s += buf;
  }
  return s;
}

std::string hex(const Frame& f) { return hex(f.bytes, f.len); }
std::string hex(const DemodFrame& f) { return hex(f.bytes, f.len); }

Frame position_frame(std::uint32_t icao, int k = 0) {
  return adsb::modes::encode_airborne_position(icao, 30000 + 25 * k,
                                               adsb::modes::CprFrame{static_cast<std::uint32_t>(1000 + 37 * k),
                                                                     static_cast<std::uint32_t>(2000 + 53 * k), (k & 1) != 0});
}

DemodConfig config_at(double fs) {
  DemodConfig c;
  c.sample_rate_hz = fs;
  return c;
}

// ---------------------------------------------------------------------------
// Magnitudes and formats
// ---------------------------------------------------------------------------

TEST(Demod, MagnitudeMethodsAgree) {
  std::vector<std::uint8_t> data(2 * 65536);
  for (int i = 0; i < 256; ++i) {
    for (int q = 0; q < 256; ++q) {
      data[static_cast<std::size_t>(2 * (i * 256 + q))] = static_cast<std::uint8_t>(i);
      data[static_cast<std::size_t>(2 * (i * 256 + q) + 1)] = static_cast<std::uint8_t>(q);
    }
  }
  std::vector<float> exact(65536), lut(65536), ambm(65536);
  magnitudes(data.data(), 65536, SampleFormat::kCu8, Magnitude::kExact, exact.data());
  magnitudes(data.data(), 65536, SampleFormat::kCu8, Magnitude::kLut, lut.data());
  magnitudes(data.data(), 65536, SampleFormat::kCu8, Magnitude::kAlphaMaxBetaMin, ambm.data());
  float worst_lut = 0, worst_lo = 0, worst_hi = 0;
  for (std::size_t k = 0; k < 65536; ++k) {
    const double fi = (static_cast<double>(k >> 8) - 127.5) / 127.5;
    const double fq = (static_cast<double>(k & 255) - 127.5) / 127.5;
    const auto ref = static_cast<float>(std::sqrt(fi * fi + fq * fq));
    ASSERT_NEAR(exact[k], ref, 1e-6f);
    worst_lut = std::max(worst_lut, std::fabs(lut[k] - ref));
    const float rel = ambm[k] / ref - 1.0f;
    worst_lo = std::min(worst_lo, rel);
    worst_hi = std::max(worst_hi, rel);
  }
  EXPECT_LT(worst_lut, 1.5f / 65535.0f * 1.4143f);  // one table step
  // Equiripple: about -3.96% on an axis, +3.96% at min/max = 0.414.
  EXPECT_GT(worst_lo, -0.0400f);
  EXPECT_LT(worst_hi, 0.0400f);
  EXPECT_LT(worst_lo, -0.035f);  // the approximation is really used
  EXPECT_GT(worst_hi, 0.035f);
}

TEST(Demod, ParsesFormatNames) {
  SampleFormat f{};
  EXPECT_TRUE(parse_format("cu8", f) && f == SampleFormat::kCu8);
  EXPECT_TRUE(parse_format("ci8", f) && f == SampleFormat::kCs8);
  EXPECT_TRUE(parse_format("ci16_le", f) && f == SampleFormat::kCs16);
  EXPECT_TRUE(parse_format("cf32_le", f) && f == SampleFormat::kCf32);
  EXPECT_FALSE(parse_format("ci16_be", f));
  EXPECT_EQ(bytes_per_sample(SampleFormat::kCs16), 4u);
}

TEST(Demod, RejectsBadConfigurations) {
  auto bad = [](auto edit) {
    DemodConfig c;
    edit(c);
    std::string why;
    return !validate(c, &why) && !why.empty();
  };
  EXPECT_TRUE(validate(DemodConfig{}));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.sample_rate_hz = 1e6; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.sample_rate_hz = std::nan(""); }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.phases = 0; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.phases = 17; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.min_signal_ratio = 0.5; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.min_pulse_quiet = 0.9; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.gate_ratio = -1; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.resume_overlap_us = 60; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.address_ttl_s = 0; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.block_samples = 100; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.first_arrival_us = -1; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.first_arrival_us = 300; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.first_arrival_match = 0; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.first_arrival_match = 1.5; }));
  EXPECT_TRUE(bad([](DemodConfig& c) { c.duplicate_window_us = -1; }));
}

// ---------------------------------------------------------------------------
// Single frames
// ---------------------------------------------------------------------------

// Every sample rate (2.4 MS/s, the RTL-SDR rate: 1.2 samples per half-bit;
// 12 MS/s: six, the general weight path), every fractional start, a carrier
// offset: the frame comes out exactly, its start within 20 ns (0.05
// samples at 2.4 MS/s).
TEST(Demod, DecodesACleanFrameAtAnyRateAndOffset) {
  for (double fs : {2.4e6, 3.2e6, 6.0e6, 12.0e6}) {
    for (int k = 0; k < 7; ++k) {
      const Frame f = position_frame(0x4840D6, k);
      const double start_us = 100.0 + k / 7.0 / (fs / 1e6);
      Burst b{f, start_us, 0.3, 0.4 * k, 25e3 * (k - 3)};
      const auto frames = demod_floats(render({b}, fs, static_cast<std::size_t>(400 * fs / 1e6), 0.01),
                                       config_at(fs));
      ASSERT_EQ(frames.size(), 1u) << "fs " << fs << " k " << k;
      EXPECT_EQ(hex(frames[0]), hex(f));
      EXPECT_EQ(frames[0].df, 17);
      EXPECT_EQ(frames[0].icao, 0x4840D6u);
      EXPECT_NEAR(frames[0].sample_pos, start_us * fs / 1e6, 0.02 * fs / 1e6) << "fs " << fs << " k " << k;  // 20 ns
      EXPECT_NEAR(frames[0].signal, 0.3f, 0.03f);
      EXPECT_GT(frames[0].snr_db, 20.0f);
      EXPECT_EQ(frames[0].corrected_bit, -1);
    }
  }
}

// At 2.0 MS/s a half-bit is exactly one sample. A frame that starts on a
// sample boundary (start = k + 0.5 in sample units, since sample k covers
// [k - 0.5, k + 0.5)) decodes with the half-bit comparison; one that starts
// at a sample centre splits every pulse evenly over two samples, so each
// sample holds half of two half-bits and the comparison cannot tell the bits
// apart (dump1090's 2 MS/s demodulator has the same blind phase). The
// sequence detector models those shared samples and still reads it.
TEST(Demod, TwoMegasamplesPerSecondHasABlindPhaseThatTheSequenceDetectorReads) {
  const double fs = 2.0e6;
  auto decoded = [&](double frac, bool sequence) {
    const Frame f = position_frame(0x4840D6, 2);
    const double start_us = (200.0 + frac) / 2.0;
    DemodConfig c = config_at(fs);
    c.sequence_detector = sequence;
    DemodStats st;
    const auto frames = demod_floats(render({{f, start_us, 0.3}}, fs, 800, 0.005), c, &st);
    return frames.size() == 1 && hex(frames[0]) == hex(f) && std::fabs(frames[0].sample_pos - (200.0 + frac)) < 0.05;
  };
  for (double frac : {0.3, 0.4, 0.5, 0.6, 0.7}) EXPECT_TRUE(decoded(frac, false)) << frac;
  EXPECT_FALSE(decoded(0.0, false));
  EXPECT_TRUE(decoded(0.0, true));
}

TEST(Demod, EveryFormatGivesTheSameFrames) {
  std::vector<Burst> bursts;
  for (int k = 0; k < 6; ++k) bursts.push_back({position_frame(0xA00001 + static_cast<std::uint32_t>(k), k), 50.0 + 150.0 * k, 0.2, 0.0, 0.0});
  const auto iq = render(bursts, 2.4e6, 2400 * 2, 0.01);
  const auto ref = demod_all(as_bytes(iq, SampleFormat::kCf32), SampleFormat::kCf32, DemodConfig{});
  ASSERT_EQ(ref.size(), 6u);
  for (SampleFormat f : {SampleFormat::kCu8, SampleFormat::kCs8, SampleFormat::kCs16}) {
    const auto got = demod_all(as_bytes(iq, f), f, DemodConfig{});
    ASSERT_EQ(got.size(), ref.size()) << format_name(f);
    for (std::size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(hex(got[i]), hex(ref[i]));
      EXPECT_NEAR(got[i].sample_pos, ref[i].sample_pos, 0.05);
      EXPECT_NEAR(got[i].sample_pos, (50.0 + 150.0 * static_cast<double>(i)) * 2.4, 0.05);
    }
  }
}

// DF11 parity may carry an interrogator code (syndrome 1..79); anything
// larger is an error.
TEST(Demod, Df11SyndromesFrom80UpAreRejected) {
  Frame bad = adsb::modes::encode_df11(0x3C6586, 5, 0);
  bad.bytes[5] ^= 0x01;  // syndrome 0x100
  Frame edge = adsb::modes::encode_df11(0x3C6587, 5, 0);
  edge.bytes[6] ^= 0x50;  // syndrome 80
  Frame ok79 = adsb::modes::encode_df11(0x3C6588, 5, 0);
  ok79.bytes[6] ^= 0x4F;  // syndrome 79
  const auto frames = demod_floats(render({{bad, 40, 0.3}, {edge, 200, 0.3}, {ok79, 300, 0.3}}, 2.4e6, 1200, 0.01), DemodConfig{});
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(hex(frames[0]), hex(ok79));
}

TEST(Demod, ShortFramesAndInterrogatorCodes) {
  const Frame df11 = adsb::modes::encode_df11(0x3C6586, 5, 0);
  const Frame df11_ii = adsb::modes::encode_df11(0x3C6586, 5, 9);  // II code 9: syndrome 9
  const auto frames = demod_floats(render({{df11, 40, 0.3}, {df11_ii, 200, 0.3}}, 2.4e6, 2400, 0.01), DemodConfig{});
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(hex(frames[0]), hex(df11));
  EXPECT_EQ(hex(frames[1]), hex(df11_ii));
  EXPECT_EQ(frames[0].len, 7);
  EXPECT_EQ(frames[1].icao, 0x3C6586u);
}

// ---------------------------------------------------------------------------
// Address/Parity frames
// ---------------------------------------------------------------------------

TEST(Demod, AddressParityFramesNeedARecentlyVerifiedAddress) {
  const Frame df4 = adsb::modes::encode_df4(0x71C011, 35000);
  const Frame df5 = adsb::modes::encode_df5(0x71C011, 0x7700);
  const Frame df11 = adsb::modes::encode_df11(0x71C011, 5);
  const Frame other = adsb::modes::encode_df4(0x123456, 1000);
  // DF4 alone: unknown address. Then DF11, after which DF4/DF5 pass; a DF4
  // from another address does not.
  DemodStats st;
  auto frames = demod_floats(render({{df4, 40, 0.3}, {df11, 200, 0.3}, {df5, 300, 0.3}, {other, 400, 0.3}, {df4, 500, 0.3}},
                                    2.4e6, 2400 * 2, 0.01),
                             DemodConfig{}, &st);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(hex(frames[0]), hex(df11));
  EXPECT_EQ(hex(frames[1]), hex(df5));
  EXPECT_EQ(frames[1].icao, 0x71C011u);
  EXPECT_EQ(hex(frames[2]), hex(df4));
  EXPECT_GE(st.unknown_address, 2u);

  // The address expires: with a 1 ms TTL a DF4 2 ms after the DF11 is dropped.
  DemodConfig short_ttl;
  short_ttl.address_ttl_s = 0.001;
  frames = demod_floats(render({{df11, 40, 0.3}, {df4, 500, 0.3}, {df4, 2500, 0.3}}, 2.4e6, 2400 * 3, 0.01), short_ttl);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[1].sample_pos < 2400, true);
}

TEST(Demod, ADf17AddressAlsoCounts) {
  const auto frames = demod_floats(render({{position_frame(0xC0FFEE), 40, 0.3},
                                           {adsb::modes::encode_df5(0xC0FFEE, 0x1200), 200, 0.3}},
                                          2.4e6, 2400, 0.01),
                                   DemodConfig{});
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[1].df, 5);
}

// ---------------------------------------------------------------------------
// Errors, noise, and what is never emitted
// ---------------------------------------------------------------------------

TEST(Demod, FixesOneWrongBitOnlyWhenAsked) {
  const Frame good = position_frame(0x4B1612, 3);
  Frame bad = good;
  bad.bytes[5] ^= 0x10;  // bit 43
  const auto iq = render({{bad, 40, 0.3}}, 2.4e6, 1200, 0.01);
  DemodStats st;
  EXPECT_TRUE(demod_floats(iq, DemodConfig{}, &st).empty());
  EXPECT_GE(st.bad_crc, 1u);
  DemodConfig fix;
  fix.fix_single_bit = true;
  const auto frames = demod_floats(iq, fix, &st);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(hex(frames[0]), hex(good));
  EXPECT_EQ(frames[0].corrected_bit, 43);
  EXPECT_EQ(st.corrected, 1u);
}

// A fixed bit inside the address: the frame reports, and whitelists, the
// corrected address, not the one received.
TEST(Demod, AFixedAddressBitGivesTheRightAddress) {
  const Frame good = position_frame(0x4B1612, 3);
  Frame bad = good;
  bad.bytes[2] ^= 0x08;  // bit 20
  const Frame df5_right = adsb::modes::encode_df5(0x4B1612, 0x1200);
  const Frame df5_wrong = adsb::modes::encode_df5(0x4B1E12, 0x1200);  // the received, wrong address
  DemodConfig fix;
  fix.fix_single_bit = true;
  const auto frames = demod_floats(render({{bad, 40, 0.3}, {df5_wrong, 200, 0.3}, {df5_right, 300, 0.3}}, 2.4e6, 1200, 0.01), fix);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(hex(frames[0]), hex(good));
  EXPECT_EQ(frames[0].icao, 0x4B1612u);
  EXPECT_EQ(frames[0].corrected_bit, 20);
  EXPECT_EQ(hex(frames[1]), hex(df5_right));
}

// The single-bit fix never touches the format bits. The one way to reach
// that: a DF19 (also parity without an overlay) with bit 3 wrong reads as a
// DF17 whose syndrome is that of one bit in the format field.
TEST(Demod, NeverFixesTheFormatBits) {
  Frame df19 = position_frame(0x4B1612, 3);
  df19.bytes[0] = static_cast<std::uint8_t>((19u << 3) | (df19.bytes[0] & 7u));
  adsb::modes::write_parity(df19.bytes, 14, 0);
  Frame received = df19;
  received.bytes[0] ^= 0x10;  // bit 3: DF19 -> DF17
  ASSERT_EQ(received.bytes[0] >> 3, 17);
  DemodConfig fix;
  fix.fix_single_bit = true;
  EXPECT_TRUE(demod_floats(render({{received, 40, 0.3}}, 2.4e6, 1200, 0.01), fix).empty());
  Frame df16 = position_frame(0x4B1612, 3);
  df16.bytes[0] ^= 0x08;  // DF17 -> DF16: an (unknown) Address/Parity frame
  EXPECT_TRUE(demod_floats(render({{df16, 40, 0.3}}, 2.4e6, 1200, 0.01), fix).empty());
}

// Noise, silence, a constant carrier, full-scale clipping and a pulse train
// that is not Mode S: nothing comes out.
TEST(Demod, NoiseAndJunkGiveNoFrames) {
  DemodConfig fix;
  fix.fix_single_bit = true;
  for (const DemodConfig& cfg : {DemodConfig{}, fix}) {
    DemodStats st;
    EXPECT_TRUE(demod_floats(render({}, 2.4e6, 2'400'000, 0.05, 7), cfg, &st).empty());
    // The filters before the bit reading keep noise cheap (measured: the
    // gate passes 1.2% of positions, 83 of them in 2.4 M pass the preamble).
    EXPECT_GT(st.gated, 0u);
    EXPECT_LT(st.gated, 2'400'000u * 25 / 1000);
    EXPECT_LT(st.preambles, 300u);
  }
  EXPECT_TRUE(demod_floats(std::vector<float>(2 * 100000, 0.0f), DemodConfig{}).empty());
  std::vector<float> carrier(2 * 100000);
  for (std::size_t k = 0; k < 100000; ++k) {
    carrier[2 * k] = static_cast<float>(0.5 * std::cos(0.01 * static_cast<double>(k)));
    carrier[2 * k + 1] = static_cast<float>(0.5 * std::sin(0.01 * static_cast<double>(k)));
  }
  EXPECT_TRUE(demod_floats(carrier, DemodConfig{}).empty());
  std::vector<std::uint8_t> clipped(2 * 200000);
  adsb::Rng rng(3);
  for (auto& b : clipped) b = (rng.next() & 1) ? 255 : 0;
  EXPECT_TRUE(demod_all(clipped, SampleFormat::kCu8, DemodConfig{}).empty());
  std::vector<float> square(2 * 100000, 0.0f);
  for (std::size_t k = 0; k < 100000; ++k) square[2 * k] = (k / 3) % 2 ? 0.4f : 0.0f;
  EXPECT_TRUE(demod_floats(square, DemodConfig{}).empty());
}

TEST(Demod, WeakFramesFadeOutAndStrongOnesAllDecode) {
  // 200 frames per level, 400 us apart (30% of the time busy).
  auto rate = [](double amp, double spacing = 400.0, const DemodConfig& cfg = DemodConfig{}) {
    std::vector<Burst> bursts;
    for (int k = 0; k < 200; ++k) bursts.push_back({position_frame(0x400000 + static_cast<std::uint32_t>(k), k), 20.0 + spacing * k, amp, 0.3 * k, 0});
    const auto frames = demod_floats(render(bursts, 2.4e6, static_cast<std::size_t>(2.4 * (spacing * 200 + 200)), 0.01, 5), cfg);
    return static_cast<double>(frames.size()) / 200.0;
  };
  // Noise sigma 0.01 per component (mean magnitude 0.0125). At 0.015 the
  // pulses are below the 2x preamble threshold. Higher up, bit errors decide.
  // At 2.4 MS/s a sample straddling two half-bits counts toward both, so the
  // half-bit comparison alone keeps only about half of the pulse amplitude as
  // contrast; the sequence detector models those samples. Measured (400
  // frames): 0.05 -> 22% / 44%, 0.06 -> 64% / 88%, 0.08 -> 96% / 99%, 0.10
  // -> 99% / 100% (comparison alone / with the detector).
  const double weak = rate(0.015), mid = rate(0.06), strong = rate(0.10);
  EXPECT_LT(weak, 0.5);
  EXPECT_GT(mid, 0.8);
  EXPECT_EQ(strong, 1.0);
  DemodConfig no_seq;
  no_seq.sequence_detector = false;
  EXPECT_LT(rate(0.06, 400.0, no_seq), mid - 0.15);  // the detector is worth it
  EXPECT_LE(weak, mid);
  // 20 us apart (86% busy): the noise floor estimate rises (few samples
  // hold only noise), but strong frames still all decode.
  EXPECT_EQ(rate(0.15, 140.0), 1.0);
}

// The sequence detector reads short (56-bit) frames too: one pass serves
// both lengths (the path through the first 56 bits does not depend on the
// rest), so weak DF11 replies gain as much as long frames do.
TEST(Demod, TheSequenceDetectorAlsoReadsShortFrames) {
  auto rate = [](bool sequence) {
    std::vector<Burst> bursts;
    for (int k = 0; k < 300; ++k) {
      bursts.push_back({adsb::modes::encode_df11(0x500000 + static_cast<std::uint32_t>(k), 5), 20.0 + 200.0 * k, 0.055, 0.3 * k, 0});
    }
    DemodConfig c;
    c.sequence_detector = sequence;
    DemodStats st;
    const auto frames = demod_floats(render(bursts, 2.4e6, static_cast<std::size_t>(2.4 * (200 * 300 + 200)), 0.01, 8), c, &st);
    for (const auto& f : frames) EXPECT_EQ(f.len, 7);
    return std::make_pair(static_cast<double>(frames.size()) / 300.0, st.sequence_decoded);
  };
  const auto [with, seq] = rate(true);
  const auto [without, none] = rate(false);
  EXPECT_EQ(none, 0u);
  EXPECT_GT(seq, 20u);
  EXPECT_GT(with, without + 0.1);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

TEST(Demod, OutputDoesNotDependOnHowTheInputIsSplit) {
  std::vector<Burst> bursts;
  adsb::Rng rng(9);
  double t = 30;
  for (int k = 0; k < 300; ++k) {
    const bool shrt = rng.uniform() < 0.3;
    const auto icao = 0x500000 + static_cast<std::uint32_t>(k % 7);
    bursts.push_back({shrt ? adsb::modes::encode_df11(icao) : position_frame(icao, k), t, rng.uniform(0.08, 0.5), rng.uniform(0, 6), 0});
    t += (shrt ? 64 : 120) + rng.uniform(0.0, 200.0);
  }
  const auto bytes = as_bytes(render(bursts, 2.4e6, static_cast<std::size_t>((t + 50) * 2.4), 0.01, 4), SampleFormat::kCu8);
  DemodConfig small;
  small.block_samples = 1024;
  const auto ref = demod_all(bytes, SampleFormat::kCu8, DemodConfig{});
  EXPECT_GT(ref.size(), 280u);
  for (std::size_t chunk : {std::size_t{1024}, std::size_t{1000}, std::size_t{777}, std::size_t{4096}, std::size_t{65536}}) {
    const auto got = demod_all(bytes, SampleFormat::kCu8, chunk <= 1024 ? small : DemodConfig{}, chunk);
    ASSERT_EQ(got.size(), ref.size()) << chunk;
    for (std::size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(hex(got[i]), hex(ref[i]));
      EXPECT_EQ(got[i].sample_pos, ref[i].sample_pos);
    }
  }
}

// Noise that changes within each 65,536-sample block, and two block sizes:
// the floor at a position comes from the blocks before it only, however
// many blocks one feed() completes (a 16-entry ring once read estimates from
// tens of milliseconds later with the default block size).
TEST(Demod, NoiseFloorDependsOnlyOnEarlierSamples) {
  std::vector<Burst> bursts;
  for (int k = 0; k < 300; ++k) bursts.push_back({position_frame(0x400000 + static_cast<std::uint32_t>(k), k), 20.0 + 400.0 * k, 0.06, 0.3 * k, 0});
  const std::size_t n = static_cast<std::size_t>(2.4 * (400 * 300 + 200));
  auto iq = render(bursts, 2.4e6, n, 0.01, 5);
  auto extra = render({}, 2.4e6, n, 0.03, 6);
  for (std::size_t k = 0; k < n; ++k) {
    if ((k % 65536) >= 32768) {
      iq[2 * k] += extra[2 * k];
      iq[2 * k + 1] += extra[2 * k + 1];
    }
  }
  const auto bytes = as_bytes(iq, SampleFormat::kCf32);
  DemodConfig small;
  small.block_samples = 1024;
  const auto big = demod_all(bytes, SampleFormat::kCf32, DemodConfig{});
  const auto tiny = demod_all(bytes, SampleFormat::kCf32, small, 1000);
  ASSERT_GT(big.size(), 100u);
  ASSERT_EQ(big.size(), tiny.size());
  for (std::size_t i = 0; i < big.size(); ++i) {
    EXPECT_EQ(hex(big[i]), hex(tiny[i]));
    EXPECT_EQ(big[i].sample_pos, tiny[i].sample_pos);
    EXPECT_EQ(big[i].noise, tiny[i].noise);
  }
}

// The timing climb reaches further at high rates; the margins grow with it
// (ASan found writes before the template buffer at 60 and 100 MS/s).
TEST(Demod, HighSampleRates) {
  for (double fs : {25e6, 60e6, 100e6}) {
    const Frame f = position_frame(0x4840D6, 1);
    for (double frac : {0.0, 0.37, 0.81}) {
      const double start_us = 100.0 + frac / (fs / 1e6);
      const auto frames = demod_floats(render({{f, start_us, 0.3}}, fs, static_cast<std::size_t>(300 * fs / 1e6), 0.01), config_at(fs));
      ASSERT_EQ(frames.size(), 1u) << fs;
      EXPECT_EQ(hex(frames[0]), hex(f));
      EXPECT_NEAR(frames[0].sample_pos, start_us * fs / 1e6, 0.02 * fs / 1e6);
    }
  }
}

TEST(Demod, NonFiniteSamplesCountAsSilence) {
  const Frame f = position_frame(0x4840D6, 1);
  auto iq = render({{f, 100.0, 0.3}, {f, 1000.0, 0.3}}, 2.4e6, 4800, 0.01);
  iq[2 * 2000] = std::numeric_limits<float>::quiet_NaN();
  iq[2 * 2100 + 1] = std::numeric_limits<float>::infinity();
  iq[2 * 2200] = -std::numeric_limits<float>::infinity();
  // One NaN in every noise window of the first block: unclamped, every
  // window mean (and so the floor and the gate) would be NaN.
  for (std::size_t k = 0; k < 1024; k += 16) iq[2 * k + 1] = std::numeric_limits<float>::quiet_NaN();
  const auto frames = demod_floats(iq, DemodConfig{});
  ASSERT_EQ(frames.size(), 2u);
  for (const auto& fr : frames) EXPECT_TRUE(std::isfinite(fr.noise) && std::isfinite(fr.signal));
}

// The same through feed_magnitudes(): non-finite magnitudes are silence.
TEST(Demod, NonFiniteMagnitudesCountAsSilence) {
  const Frame f = position_frame(0x4840D6, 1);
  const auto iq = render({{f, 100.0, 0.3}, {f, 1000.0, 0.3}}, 2.4e6, 4800, 0.01);
  std::vector<float> mag(iq.size() / 2);
  for (std::size_t k = 0; k < mag.size(); ++k) mag[k] = std::sqrt(iq[2 * k] * iq[2 * k] + iq[2 * k + 1] * iq[2 * k + 1]);
  for (std::size_t k = 0; k < 1024; k += 16) mag[k] = std::numeric_limits<float>::quiet_NaN();
  mag[3000] = std::numeric_limits<float>::infinity();
  Demodulator d(DemodConfig{});
  std::vector<DemodFrame> frames;
  for (std::size_t off = 0; off < mag.size(); off += 1024) {
    const auto got = d.feed_magnitudes(mag.data() + off, std::min<std::size_t>(1024, mag.size() - off));
    frames.insert(frames.end(), got.begin(), got.end());
  }
  const auto rest = d.flush();
  frames.insert(frames.end(), rest.begin(), rest.end());
  ASSERT_EQ(frames.size(), 2u);
  for (const auto& fr : frames) EXPECT_TRUE(std::isfinite(fr.noise) && std::isfinite(fr.signal));
}

TEST(Demod, FlushFindsAFrameAtTheEndButNotACutOne) {
  const Frame f = position_frame(0x111111);
  // Ends 2 us before the end of the data: found only by flush().
  std::size_t n = static_cast<std::size_t>((100 + 120 + 2) * 2.4);
  auto bytes = as_bytes(render({{f, 100, 0.3}}, 2.4e6, n, 0.01), SampleFormat::kCf32);
  Demodulator d(DemodConfig{});
  EXPECT_TRUE(d.feed(bytes.data(), n, SampleFormat::kCf32).empty());
  const auto rest = d.flush();
  ASSERT_EQ(rest.size(), 1u);
  EXPECT_EQ(hex(rest[0]), hex(f));
  // Cut 10 us short: nothing.
  n = static_cast<std::size_t>((100 + 110) * 2.4);
  bytes = as_bytes(render({{f, 100, 0.3}}, 2.4e6, n, 0.01), SampleFormat::kCf32);
  EXPECT_TRUE(demod_all(bytes, SampleFormat::kCf32, DemodConfig{}).empty());
}

// A reply that starts under the last microseconds of the previous one (a
// busy channel). The first frame's last four bits are 1, 1, x, 0, so the
// second's preamble pulses (0, 1, 3.5 us) land on pulses it already has
// and both frames stay intact; with resume_overlap_us = 0 the scan jumps
// past the second preamble.
TEST(Demod, FindsAReplyStartingUnderThePreviousOnesTail) {
  Frame a;
  bool found = false;
  for (int k = 0; k < 4000 && !found; ++k) {
    a = position_frame(0x7C1234, k);
    const std::uint8_t last = a.bytes[13];
    found = (last & 0x0D) == 0x0C;  // bits 108, 109 = 1, bit 111 = 0
  }
  ASSERT_TRUE(found);
  const Frame b = position_frame(0x7C9999, 1);
  const double a_start = 40.0, b_start = a_start + 120.0 - 4.0;
  const auto iq = render({{a, a_start, 0.3}, {b, b_start, 0.3}}, 2.4e6, 2400, 0.005);
  const auto frames = demod_floats(iq, DemodConfig{});
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(hex(frames[0]), hex(a));
  EXPECT_EQ(hex(frames[1]), hex(b));
  DemodConfig no_overlap;
  no_overlap.resume_overlap_us = 0;
  const auto lost = demod_floats(iq, no_overlap);
  ASSERT_EQ(lost.size(), 1u);
  EXPECT_EQ(hex(lost[0]), hex(a));
}

// 20 frames without a gap (2.4 ms, shorter than the 8 noise blocks the
// floor looks back over), after 4 ms of noise.
TEST(Demod, BackToBackFramesAllDecode) {
  std::vector<Burst> bursts;
  double t = 4000;
  for (int k = 0; k < 20; ++k) {
    bursts.push_back({position_frame(0x600000 + static_cast<std::uint32_t>(k), k), t, 0.3, 0.7 * k, 0});
    t += 120.0;  // the next preamble starts where this frame ends
  }
  EXPECT_EQ(demod_floats(render(bursts, 2.4e6, static_cast<std::size_t>((t + 20) * 2.4), 0.01), DemodConfig{}).size(), 20u);
}

TEST(Demod, BeastOutputRoundTrips) {
  DemodFrame f;
  const Frame src = position_frame(0x1A1A1A);  // 0x1A bytes must be escaped
  std::memcpy(f.bytes, src.bytes, 14);
  f.len = 14;
  f.sample_pos = 0x1A1A1A / 5.0 + 1000.2;  // at 2.4 MS/s: 5 ticks per sample
  f.signal = 0.5f;
  std::vector<std::uint8_t> out;
  const std::size_t n = to_beast(f, 2.4e6, out);
  EXPECT_EQ(n, out.size());
  adsb::ingest::BeastParser p;
  int got = 0;
  p.feed(out.data(), out.size(), [&](const adsb::ingest::BeastFrame& bf) {
    ++got;
    EXPECT_EQ(bf.len, 14);
    EXPECT_EQ(std::memcmp(bf.payload, src.bytes, 14), 0);
    EXPECT_EQ(bf.timestamp, static_cast<std::uint64_t>(std::llround(f.sample_pos * 5.0)));
    EXPECT_EQ(bf.signal, 128);
  });
  EXPECT_EQ(got, 1);
}

// ---------------------------------------------------------------------------
// Scenarios from the IQ generator
// ---------------------------------------------------------------------------

struct Scenario {
  adsb::sim::IqPlan plan;
  std::vector<std::uint8_t> cu8;
};

Scenario scenario(int aircraft, double seconds, std::uint64_t seed, double noise_figure_db = 5.0) {
  adsb::sim::SynthConfig syn;
  syn.seed = seed;
  syn.aircraft = aircraft;
  syn.duration_s = seconds;
  syn.keep_transmissions = true;
  const auto traffic = adsb::sim::generate(syn);
  adsb::sim::IqConfig cfg;
  cfg.seed = seed;
  cfg.noise_figure_db = noise_figure_db;
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

struct Score {
  std::size_t clean = 0, clean_hit = 0, matched = 0, false_frames = 0, false_df17 = 0;
  double max_timing = 0.0;
};

// Each truth message is matched by its bytes and a start within 4 samples.
Score score(const adsb::sim::IqPlan& plan, const std::vector<DemodFrame>& frames, double snr_lo, double snr_hi) {
  Score s;
  std::vector<bool> used(plan.messages.size(), false);
  for (const DemodFrame& f : frames) {
    std::size_t best = plan.messages.size();
    double best_d = 4.0;
    for (std::size_t i = 0; i < plan.messages.size(); ++i) {
      const auto& m = plan.messages[i];
      const double d = std::fabs(m.sample_pos - f.sample_pos);
      if (!used[i] && d <= best_d && m.len == f.len && std::memcmp(m.bytes, f.bytes, f.len) == 0) {
        best = i;
        best_d = d;
      }
    }
    if (best == plan.messages.size()) {
      ++s.false_frames;
      if (f.df == 17) ++s.false_df17;
    } else {
      used[best] = true;
      ++s.matched;
      s.max_timing = std::max(s.max_timing, best_d);
    }
  }
  for (std::size_t i = 0; i < plan.messages.size(); ++i) {
    const auto& m = plan.messages[i];
    if (!m.collided && m.snr_db >= snr_lo && m.snr_db < snr_hi) {
      ++s.clean;
      if (used[i]) ++s.clean_hit;
    }
  }
  return s;
}

TEST(DemodScenario, DecodesTheGeneratorsTrafficLikeReadsb) {
  const Scenario sc = scenario(80, 3.0, 31);
  const auto frames = demod_all(sc.cu8, SampleFormat::kCu8, DemodConfig{});
  const Score s = score(sc.plan, frames, 15.0, 30.0);
  ASSERT_GT(s.clean, 800u);
  EXPECT_GE(static_cast<double>(s.clean_hit) / static_cast<double>(s.clean), 0.97);
  EXPECT_EQ(s.false_df17, 0u);
  EXPECT_LE(s.false_frames, 10u);
  EXPECT_LT(s.max_timing, 0.5);
}

TEST(DemodScenario, MagnitudeApproximationsCostLittle) {
  const Scenario sc = scenario(60, 2.0, 32);
  DemodConfig exact, lut, ambm;
  lut.magnitude = Magnitude::kLut;
  ambm.magnitude = Magnitude::kAlphaMaxBetaMin;
  const Score se = score(sc.plan, demod_all(sc.cu8, SampleFormat::kCu8, exact), 15.0, 30.0);
  const Score sl = score(sc.plan, demod_all(sc.cu8, SampleFormat::kCu8, lut), 15.0, 30.0);
  const Score sa = score(sc.plan, demod_all(sc.cu8, SampleFormat::kCu8, ambm), 15.0, 30.0);
  EXPECT_LE(std::max(se.clean_hit, sl.clean_hit) - std::min(se.clean_hit, sl.clean_hit), 2u);
  EXPECT_GE(static_cast<double>(sa.clean_hit), 0.99 * static_cast<double>(se.clean_hit));
  EXPECT_EQ(sa.false_df17, 0u);
}

// The output is a pure function of the input: frames, positions and levels
// hash the same on every platform (float arithmetic with -ffp-contract=off,
// no libm in the decisions).
// Every bit of every output field (%a: the exact float), plus the counts
// of the decisions on the way (positions gated, preambles, detector
// decodes, failed checks): a change that moves any value or any decision,
// even by one rounding step, changes the hash. Equal on macOS Clang and
// Linux GCC / Clang, x86-64 and ARM64.
std::string pin_of(const std::vector<DemodFrame>& frames, const DemodStats& st) {
  std::string all;
  for (const DemodFrame& f : frames) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s %a %a %a %a\n", hex(f).c_str(), f.sample_pos, static_cast<double>(f.signal),
                  static_cast<double>(f.margin), static_cast<double>(f.noise));
    all += buf;
  }
  all += std::to_string(st.gated) + " " + std::to_string(st.preambles) + " " + std::to_string(st.sequence_decoded) + " " +
         std::to_string(st.bad_crc) + " " + std::to_string(st.unknown_address);
  adsb::Sha256 sha;
  sha.update(all.data(), all.size());
  return adsb::Sha256::hex(sha.finish()).substr(0, 16) + " " + std::to_string(frames.size());
}

TEST(DemodScenario, OutputIsPinned) {
  const Scenario sc = scenario(40, 1.0, 33);
  DemodStats st;
  const auto frames = demod_all(sc.cu8, SampleFormat::kCu8, DemodConfig{}, 0, &st);
  EXPECT_EQ(pin_of(frames, st), "7f3519b88bfb0b0f 183");
}

// Weak signals (noise figure 22 dB): many preambles fail the half-bit
// reading, so the sequence detector decides a good share of the frames.
TEST(DemodScenario, WeakSignalOutputIsPinned) {
  const Scenario sc = scenario(60, 1.0, 34, 22.0);
  DemodStats st;
  const auto frames = demod_all(sc.cu8, SampleFormat::kCu8, DemodConfig{}, 0, &st);
  EXPECT_GT(st.sequence_decoded, 5u);
  EXPECT_EQ(pin_of(frames, st), "a65c4e5264a2da9c 48");
}

// ---------------------------------------------------------------------------
// Echoes (step 6)
// ---------------------------------------------------------------------------

struct EchoCase {
  Frame frame;
  double start_us = 0.0;   // direct path
  double delay_us = 0.0;   // echo after it
  double amp = 0.0;        // direct path's amplitude
};

// Frames with an echo 1.5-30 us later at a random carrier phase, 0.3-0.6 or
// (a direct path partly blocked) 1.6-3 times as strong: the copies garble
// each other, and the stronger one may decode.
std::vector<EchoCase> echo_cases(std::vector<Burst>& bursts, int n, std::uint64_t seed) {
  adsb::Rng rng(seed);
  std::vector<EchoCase> cases;
  double t = 40;
  for (int k = 0; k < n; ++k) {
    const Frame f = position_frame(0x710000 + static_cast<std::uint32_t>(k), k);
    const double delay = rng.uniform(1.5, 30.0);
    const double amp0 = rng.uniform(0.15, 0.3);
    const double ratio = k % 2 ? rng.uniform(0.3, 0.6) : rng.uniform(1.6, 3.0);
    const double amp = k % 2 ? amp0 : amp0 / ratio;
    const double ph = rng.uniform(0, 6.283);
    bursts.push_back({f, t, amp, ph, 0});
    bursts.push_back({f, t + delay, amp * ratio, ph + rng.uniform(1.0, 5.0), 0});
    cases.push_back({f, t, delay, amp});
    t += 400;
  }
  return cases;
}

const EchoCase* case_of(const std::vector<EchoCase>& cases, const DemodFrame& f) {
  for (const EchoCase& c : cases) {
    if (hex(c.frame) == hex(f)) return &c;
  }
  return nullptr;
}

// When the echo is what decodes, the frame takes the direct path's time:
// with step 6 every decoded frame is within 0.1 us of its direct path,
// except a few with echoes under 5 us (most of the direct path's samples
// are under the echo's pulses); with it off, many carry their echo's time.
void check_echo_timing(double fs) {
  SCOPED_TRACE(fs);
  std::vector<Burst> bursts;
  const auto cases = echo_cases(bursts, 120, 21);
  const double us = fs / 1e6;
  const auto iq = render(bursts, fs, static_cast<std::size_t>((cases.back().start_us + 400) * us), 0.01, 5);
  DemodConfig off = config_at(fs);
  off.first_arrival_us = 0;
  DemodStats st_on, st_off;
  const auto on = demod_floats(iq, config_at(fs), &st_on);
  const auto plain = demod_floats(iq, off, &st_off);
  EXPECT_EQ(on.size(), plain.size());
  // (2.0 MS/s has a blind phase: fewer frames decode at all.)
  EXPECT_GE(on.size(), fs == 2.0e6 ? 45u : 85u);
  int at_echo = 0, retimed = 0;
  for (const DemodFrame& f : on) {
    const EchoCase* c = case_of(cases, f);
    ASSERT_NE(c, nullptr) << hex(f);
    if ((c->delay_us >= 5.0 && fs != 2.0e6) || f.echo_delay_us > 0) {
      EXPECT_NEAR(f.sample_pos, c->start_us * us, 0.1 * us) << "delay " << c->delay_us;
    }
    if (f.echo_delay_us > 0) {
      ++retimed;
      EXPECT_NEAR(f.echo_delay_us, c->delay_us, 0.1) << hex(f);
      // The direct path's amplitude, not the echo's (which differs by a
      // factor 1.6-3 or 0.3-0.6).
      EXPECT_NEAR(f.signal, c->amp, 0.25 * c->amp) << hex(f);
    }
  }
  for (const DemodFrame& f : plain) {
    const EchoCase* c = case_of(cases, f);
    ASSERT_NE(c, nullptr);
    if (std::abs(f.sample_pos - (c->start_us + c->delay_us) * us) < 0.1 * us) ++at_echo;
    EXPECT_EQ(f.echo_delay_us, 0.0f);
  }
  // At 2.0 MS/s (a half-bit is one sample) the correlation of a direct
  // path is lower and fewer pass first_arrival_match: about 30%.
  EXPECT_GT(at_echo, 30);
  EXPECT_GE(retimed, fs == 2.0e6 ? at_echo / 4 : at_echo * 9 / 10);
  EXPECT_EQ(st_on.retimed, static_cast<std::uint64_t>(retimed));
  EXPECT_EQ(st_off.retimed, 0u);
}

TEST(Demod, AFrameDecodedFromItsEchoTakesTheDirectPathsTime) {
  for (double fs : {2.4e6, 2.0e6, 6.0e6, 12.0e6}) check_echo_timing(fs);
}

// Without echoes nothing is moved, also when replies from other aircraft
// overlap (a reply's preamble that failed under another one has other bits).
TEST(Demod, OverlappingRepliesFromOtherAircraftAreNotTakenForEchoes) {
  std::vector<Burst> bursts;
  adsb::Rng rng(31);
  double t = 30;
  for (int k = 0; k < 400; ++k) {
    const bool shrt = rng.uniform() < 0.3;
    const auto icao = 0x600000 + static_cast<std::uint32_t>(k);
    bursts.push_back({shrt ? adsb::modes::encode_df11(icao) : position_frame(icao, k), t, rng.uniform(0.05, 0.5),
                      rng.uniform(0, 6), 0});
    t += rng.uniform(3.0, 150.0);  // many overlaps
  }
  const double us = 2.4;
  const auto iq = render(bursts, 2.4e6, static_cast<std::size_t>((t + 200) * us), 0.01, 6);
  DemodStats st;
  const auto frames = demod_floats(iq, DemodConfig{}, &st);
  EXPECT_GT(frames.size(), 80u);
  EXPECT_EQ(st.retimed, 0u);
  int real = 0;
  for (const DemodFrame& f : frames) {
    // (DF11 near-misses with an interrogator code are not among the bursts.)
    const Burst* b = nullptr;
    for (const Burst& x : bursts) {
      if (hex(x.frame) == hex(f)) b = &x;
    }
    if (b == nullptr) continue;
    ++real;
    EXPECT_NEAR(f.sample_pos, b->start_us * us, 0.1 * us) << hex(f);
  }
  EXPECT_GT(real, 80);
}

// An echo later than the frame is long decodes as a second copy: dropped
// within the window; a real repeat after it is kept.
TEST(Demod, ACopyOfARecentFrameIsDroppedAsAnEcho) {
  const Frame f = position_frame(0x4840D6, 3);
  const double fs = 2.4e6, us = fs / 1e6;
  const std::vector<Burst> bursts = {{f, 50, 0.3, 0, 0}, {f, 50 + 150, 0.2, 1.0, 0}, {f, 50 + 600, 0.3, 2.0, 0}};
  const auto iq = render(bursts, fs, static_cast<std::size_t>(800 * us), 0.01, 7);
  DemodStats st;
  const auto frames = demod_floats(iq, DemodConfig{}, &st);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_NEAR(frames[0].sample_pos, 50 * us, 0.1 * us);
  EXPECT_NEAR(frames[1].sample_pos, 650 * us, 0.1 * us);
  EXPECT_EQ(st.duplicates, 1u);
  DemodConfig keep;
  keep.duplicate_window_us = 0;
  DemodStats st0;
  const auto all = demod_floats(iq, keep, &st0);
  ASSERT_EQ(all.size(), 3u);
  EXPECT_NEAR(all[1].sample_pos, 200 * us, 0.1 * us);
  EXPECT_EQ(st0.duplicates, 0u);
  // A different frame at the echo's place is not a copy.
  const std::vector<Burst> other = {{f, 50, 0.3, 0, 0}, {position_frame(0x4840D6, 4), 200, 0.2, 1.0, 0}};
  EXPECT_EQ(demod_floats(render(other, fs, static_cast<std::size_t>(400 * us), 0.01, 8), DemodConfig{}).size(), 2u);
}

// Step 6 looks back into samples an earlier feed() delivered: the output
// still does not depend on how the input is split.
TEST(Demod, EchoHandlingDoesNotDependOnHowTheInputIsSplit) {
  std::vector<Burst> bursts;
  const auto cases = echo_cases(bursts, 60, 41);
  const auto bytes = as_bytes(render(bursts, 2.4e6, static_cast<std::size_t>((cases.back().start_us + 400) * 2.4), 0.01, 9),
                              SampleFormat::kCu8);
  DemodConfig small;
  small.block_samples = 1024;
  DemodStats st;
  const auto ref = demod_all(bytes, SampleFormat::kCu8, DemodConfig{}, 0, &st);
  EXPECT_GT(st.retimed, 5u);
  for (std::size_t chunk : {std::size_t{1024}, std::size_t{777}, std::size_t{5000}}) {
    const auto got = demod_all(bytes, SampleFormat::kCu8, chunk <= 1024 ? small : DemodConfig{}, chunk);
    ASSERT_EQ(got.size(), ref.size()) << chunk;
    for (std::size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(hex(got[i]), hex(ref[i]));
      EXPECT_EQ(got[i].sample_pos, ref[i].sample_pos);
      EXPECT_EQ(got[i].echo_delay_us, ref[i].echo_delay_us);
    }
  }
}

}  // namespace
