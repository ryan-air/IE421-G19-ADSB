#pragma once

// Carrier frequency of a decoded Mode S message, from its IQ samples
// (transponder frequency-stability study, plan §22.5).
//
// Once a message is decoded, its pulse pattern is known: the four preamble
// pulses and, for each data bit, which half-bit holds the pulse. A sample k
// covers [k - 0.5, k + 0.5) (the demodulator's convention); the share of it
// that pulses cover is its weight w_k, and the middle of the covered part is
// its time t_k. For a carrier that is coherent across the message, sample k
// is about A w_k exp(j(phi + 2 pi f t_k)) plus noise, and the maximum-
// likelihood estimate of f maximizes |sum_k w_k z_k exp(-j 2 pi f t_k)|.
//
//  1. Coarse: the peak of a zero-padded FFT of the weighted samples (their
//     periodogram on the sample grid, over the whole band +-fs/2).
//  2. Grid: the same sum on the samples' covered middles, on a grid of
//     1/(4 T) (T = the message's span) over two FFT bins on each side of the
//     peak.
//  3. Fine: weighted least squares of the remaining phase of each sample
//     against its time, a few times; the last fit's residuals give the
//     standard error of f.
//
// `coherence` = |sum| / sum |terms| at the estimate: 1 for a clean coherent
// carrier, lower with noise, and low when the phase jumps between pulses (a
// transmitter that is not phase-coherent, or a collision); `phase_rms_rad`
// is the residual phase spread. Clipped samples (CarrierConfig::clip_level)
// and non-finite ones (a caller's mark for "unusable") are left out. An
// estimate whose fine fit moves more than two grid steps from the grid's
// peak is rejected (ok = false).
//
// The estimate is the baseband frequency as the recording's sample clock
// sees it: transmitter offset + Doppler - receiver LO error, all divided by
// (1 + sample-clock error). Removing Doppler and the receiver's error is the
// caller's job (adsb-carrier, scripts/carrier-study.py).
//
// No allocation after construction.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "adsb/demod/demod.hpp"

namespace adsb::demod {

struct CarrierConfig {
  double sample_rate_hz = 2.4e6;
  double min_cover = 0.5;       // samples less covered by pulses than this are not used
  int fine_iterations = 4;      // least-squares refinements (1..16)
  int min_samples = 16;         // fewer usable samples: no estimate
  // Samples with |I| or |Q| at or above this (relative to full scale) are
  // clipped by the ADC and not used: clipping each component separately
  // bends the phase. > 1 uses every sample.
  double clip_level = 0.99;
};

[[nodiscard]] bool validate(const CarrierConfig& cfg, std::string* why = nullptr);

struct CarrierEstimate {
  bool ok = false;
  double freq_hz = 0.0;         // baseband carrier frequency
  double sigma_hz = 0.0;        // standard error (from the fit's residuals)
  double coherence = 0.0;       // 0..1
  double phase_rms_rad = 0.0;   // residual phase spread
  double coarse_hz = 0.0;       // step 1's value (diagnostics)
  int samples = 0;              // samples used
  int clipped = 0;              // pulse samples left out as clipped
};

// Interleaved float I/Q, relative to full scale (the same scale as
// magnitudes()): n complex samples of `data` into out[2n].
void to_complex(const std::uint8_t* data, std::size_t n, SampleFormat f, float* out) noexcept;

class CarrierEstimator {
 public:
  explicit CarrierEstimator(const CarrierConfig& cfg);

  // iq: interleaved I/Q of absolute samples [base, base + n). The message
  // starts (preamble) at sample_pos and has `len` bytes (7 or 14). Returns
  // ok = false when the message is not wholly inside the samples or too few
  // samples are usable.
  [[nodiscard]] CarrierEstimate estimate(const float* iq, std::int64_t base, std::size_t n, double sample_pos,
                                         const std::uint8_t* bytes, int len);
  [[nodiscard]] CarrierEstimate estimate(const float* iq, std::int64_t base, std::size_t n, const DemodFrame& f) {
    return estimate(iq, base, n, f.sample_pos, f.bytes, f.len);
  }

  // Pulse intervals (in us from the preamble start) of a message: preamble,
  // then one per data bit. Returns their number (4 + 8 len); out holds 116.
  static int pulse_intervals_us(const std::uint8_t* bytes, int len, double* a, double* b) noexcept;

  [[nodiscard]] const CarrierConfig& config() const noexcept { return cfg_; }

 private:
  CarrierConfig cfg_;
  double spu_ = 2.4;            // samples per microsecond
  // Per used sample: weighted value (w z) and time (s, from the preamble start).
  std::vector<double> re_, im_, t_, w_;
  std::vector<std::int64_t> idx_;
  std::vector<double> fft_re_, fft_im_, tw_re_, tw_im_;  // coarse search
  void fft(std::size_t n) noexcept;
};

}  // namespace adsb::demod
