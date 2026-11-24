// Fuzzes the carrier frequency estimator (adsb/demod/carrier.hpp) with
// arbitrary samples, frame bytes and positions.
//
// Input layout: byte 0 selects the options (sample rate, 7 or 14 bytes,
// min_cover, clip level), bytes 1-4 the preamble position (in 1/256
// samples), bytes 5-18 the frame; the rest is cs8 I/Q.
//
// Invariants:
//  * no crash / UB, no reads outside the samples (ASan): a message not
//    wholly inside them gives ok = false;
//  * an estimate has finite values, |f| within the band (plus the fine
//    search's reach), sigma >= 0, coherence in [0, 1], enough samples;
//  * the same samples handed over from another base give the same result.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "adsb/demod/carrier.hpp"
#include "fuzz_check.hpp"

using namespace adsb::demod;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 19) return 0;
  const std::uint8_t opts = data[0];
  const double rates[4] = {2.4e6, 2.0e6, 6.0e6, 12.0e6};
  CarrierConfig cfg;
  cfg.sample_rate_hz = rates[opts & 3];
  const int len = (opts & 4) ? 7 : 14;
  cfg.min_cover = (opts & 8) ? 0.9 : 0.5;
  cfg.clip_level = (opts & 16) ? 0.5 : 0.99;
  cfg.fine_iterations = 1 + ((opts >> 5) & 7);
  std::uint32_t raw_pos;
  std::memcpy(&raw_pos, data + 1, 4);
  const double pos = static_cast<double>(raw_pos % (1u << 20)) / 256.0 - 8.0;  // may start before the samples
  std::uint8_t bytes[14];
  std::memcpy(bytes, data + 5, 14);
  const std::size_t n = (size - 19) / 2;
  std::vector<float> iq(2 * n + 2);
  to_complex(data + 19, n, SampleFormat::kCs8, iq.data());

  CarrierEstimator est(cfg);
  const CarrierEstimate e = est.estimate(iq.data(), 0, n, pos, bytes, len);
  if (e.ok) {
    FUZZ_CHECK(std::isfinite(e.freq_hz) && std::isfinite(e.sigma_hz) && std::isfinite(e.coherence));
    FUZZ_CHECK(std::fabs(e.freq_hz) <= cfg.sample_rate_hz);
    FUZZ_CHECK(e.sigma_hz >= 0.0);
    FUZZ_CHECK(e.coherence >= 0.0 && e.coherence <= 1.0 + 1e-9);
    FUZZ_CHECK(e.samples >= cfg.min_samples);
  }
  // From another base: the same samples, the same answer.
  if (n > 4) {
    const auto shifted = est.estimate(iq.data() + 4, 2, n - 2, pos, bytes, len);
    if (e.ok && std::floor(pos + 0.5) >= 2.0) {  // the first pulse sample is still inside
      FUZZ_CHECK(shifted.ok);
      FUZZ_CHECK(shifted.freq_hz == e.freq_hz && shifted.sigma_hz == e.sigma_hz);
    }
  }
  return 0;
}
