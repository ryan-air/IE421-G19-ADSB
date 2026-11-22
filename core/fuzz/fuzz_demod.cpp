// Fuzzes the Mode S demodulator with arbitrary sample streams (noise,
// clipping, junk: plan §5.3).
//
// Input layout: byte 0 selects the options (format, sample rate up to
// 100 MS/s, single-bit fix, sequence detector, resume overlap), byte 1 the
// chunk size; the rest is the sample
// data. Small pulses are mixed in by the fuzzer itself as it learns.
//
// Invariants:
//  * no crash / UB, no reads outside the buffers (ASan);
//  * every DF17/18 frame has a zero CRC syndrome (or exactly one bit was
//    fixed, when fixing is on), every DF11 a syndrome below 80;
//  * every Address/Parity frame (DF0/4/5/16/20/21) carries, as its parity,
//    an address that an earlier DF11/17/18 frame in the same stream showed;
//  * frames come out in time order, inside the stream, with finite levels;
//  * the output does not depend on the chunk size.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "adsb/demod/demod.hpp"
#include "adsb/modes/crc24.hpp"
#include "fuzz_check.hpp"

using namespace adsb::demod;

namespace {

std::vector<DemodFrame> run(const std::uint8_t* data, std::size_t n_samples, SampleFormat f, const DemodConfig& cfg,
                            std::size_t chunk) {
  Demodulator d(cfg);
  std::vector<DemodFrame> out;
  const std::size_t bps = bytes_per_sample(f);
  for (std::size_t off = 0; off < n_samples; off += chunk) {
    const auto got = d.feed(data + off * bps, std::min(chunk, n_samples - off), f);
    out.insert(out.end(), got.begin(), got.end());
  }
  const auto rest = d.flush();
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) return 0;
  const std::uint8_t opts = data[0];
  const SampleFormat formats[4] = {SampleFormat::kCu8, SampleFormat::kCs8, SampleFormat::kCs16, SampleFormat::kCf32};
  const SampleFormat f = formats[opts & 3];
  const double rates[8] = {2.4e6, 2.0e6, 3.2e6, 12.0e6, 2.4e6, 2.4e6, 40e6, 100e6};
  DemodConfig cfg;
  cfg.sample_rate_hz = rates[((opts >> 2) & 3) | ((opts >> 4) & 4)];
  cfg.fix_single_bit = (opts & 0x10) != 0;
  cfg.sequence_detector = (opts & 0x20) == 0;
  cfg.resume_overlap_us = (opts & 0x80) ? 56.0 : 16.0;
  cfg.block_samples = 1024;
  const std::size_t chunk = 1 + (static_cast<std::size_t>(data[1]) * 4);  // 1..1021 samples
  data += 2;
  size -= 2;
  const std::size_t n = size / bytes_per_sample(f);
  if (n == 0) return 0;
  // cf32 input may hold NaN / inf: the demodulator must not crash, and every
  // frame it emits must still pass the checks.
  const auto frames = run(data, n, f, cfg, chunk);

  std::set<std::uint32_t> verified;
  double last = -1e300;
  for (const DemodFrame& fr : frames) {
    FUZZ_CHECK(fr.len == 7 || fr.len == 14);
    const unsigned df = fr.bytes[0] >> 3;
    FUZZ_CHECK(df == fr.df);
    FUZZ_CHECK((df >= 16) == (fr.len == 14));
    const std::uint32_t syn = adsb::modes::crc24_syndrome(fr.bytes, fr.len);
    if (df == 17 || df == 18) {
      FUZZ_CHECK(syn == 0);
      FUZZ_CHECK(fr.corrected_bit < 0 || cfg.fix_single_bit);
      verified.insert(fr.icao);
    } else if (df == 11) {
      FUZZ_CHECK(syn < 80);
      verified.insert(fr.icao);
    } else {
      FUZZ_CHECK(df == 0 || df == 4 || df == 5 || df == 16 || df == 20 || df == 21);
      FUZZ_CHECK(fr.icao == syn);
      FUZZ_CHECK(verified.count(syn) == 1);
    }
    FUZZ_CHECK(std::isfinite(fr.sample_pos) && fr.sample_pos >= 0.0 && fr.sample_pos < static_cast<double>(n));
    FUZZ_CHECK(fr.sample_pos >= last);
    last = fr.sample_pos;
    FUZZ_CHECK(std::isfinite(fr.signal) && std::isfinite(fr.noise));
  }

  // Chunking does not matter: the fuzzer's chunk size with 1024-sample
  // blocks vs a 65,536-sample block size fed whole (many noise blocks per
  // feed).
  if (n <= 400000) {
    DemodConfig big = cfg;
    big.block_samples = 1 << 16;
    const auto again = run(data, n, f, big, big.block_samples);
    FUZZ_CHECK(again.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
      FUZZ_CHECK(std::memcmp(again[i].bytes, frames[i].bytes, 14) == 0);
      FUZZ_CHECK(again[i].sample_pos == frames[i].sample_pos);
      FUZZ_CHECK(again[i].noise == frames[i].noise || (std::isnan(again[i].noise) && std::isnan(frames[i].noise)));
    }
  }
  return 0;
}
