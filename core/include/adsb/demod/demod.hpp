#pragma once

// Mode S demodulator: complex baseband samples -> CRC-checked frames
// (plan §5.2, Workstream A).
//
// Input is a stream of complex samples at any rate >= 2 MS/s (RTL-SDR
// recordings are 2.0 or 2.4 MS/s) in the usual formats (cu8, cs8, cs16,
// cf32). Magnitudes are taken first, so a carrier offset (transmitter
// tolerance, Doppler, receiver ppm) does not matter.
//
// Algorithm, per sample position t (in samples from the stream start):
//  1. Gate: for each of the four preamble pulses, one of the two windows of
//     floor(chip) + 1 samples that can hold it (for any start in [t, t+1))
//     must sum to gate_ratio of what the weakest accepted pulse gives. Noise
//     passes this ~1% of the time; it is computed for a whole block at once
//     with branch-free loops (vectorized).
//  2. Preamble: for `phases` fractional starts s = t + k/phases, the mean
//     magnitude of the four preamble pulses (0, 1, 3.5, 4.5 us; 0.5 us each)
//     is compared with the quiet parts of the preamble and with the noise
//     floor. A sample covers [k - 0.5, k + 0.5); a pulse's energy is the
//     integral of the piecewise-constant magnitude over the pulse interval,
//     so any sample rate works (2.4 MS/s gives 1.2 samples per half-bit).
//  3. Bits: each data bit is 1 if its first half holds more energy than its
//     second half. The downlink format (first 5 bits) gives the length.
//     At 2.4 MS/s a sample can straddle two half-bits and counts toward
//     both, which costs contrast; when this reading fails the check (3b), a
//     sequence detector fits the preamble's pulse level and noise floor and
//     finds the most likely bits with a 2-state Viterbi over a model of
//     every sample (Impl::detect_sequence).
//  4. Check: DF17/18 need a zero CRC syndrome, DF11 a syndrome below 80
//     (the interrogator code), and the Address/Parity formats (DF0/4/5/16/
//     20/21, whose parity is the address) an address that a verified DF11/17/
//     18 frame showed within `address_ttl_s`. Optionally, a single wrong bit
//     of a DF11/17/18 frame is corrected (readsb's --fix); off by default.
//     The phase whose frame passes with the best bit margins wins.
//  5. Timing: the winner's start is refined with a matched filter (the
//     expected sample magnitudes for a trial start, normalized; a hill
//     climb in 0.04 us steps from the winning phase, then a parabola
//     through the peak), which also gives the pulse amplitude. Scanning
//     resumes 16 us before the message's end, since the next reply may
//     start under it.
//
// No allocation after construction; feed() takes at most `block_samples`
// per call and returns the frames found so far (a frame near the end of the
// samples is held back until the next call or flush()).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace adsb::demod {

enum class SampleFormat : std::uint8_t {
  kCu8,   // unsigned 8-bit I/Q, 127.5 = zero (RTL-SDR, readsb UC8)
  kCs8,   // signed 8-bit
  kCs16,  // signed 16-bit little-endian (readsb SC16)
  kCf32,  // 32-bit float little-endian, full scale 1.0
};

[[nodiscard]] std::size_t bytes_per_sample(SampleFormat f) noexcept;  // per complex sample
[[nodiscard]] const char* format_name(SampleFormat f) noexcept;
// Accepts cu8/cs8/cs16/cf32 and the SigMF names (ci8, ci16_le, cf32_le).
[[nodiscard]] bool parse_format(const std::string& s, SampleFormat& out) noexcept;

enum class Magnitude : std::uint8_t {
  kExact,  // sqrt(i^2 + q^2) (vectorizes: one sqrt per sample)
  kLut,    // cu8 only: 64K-entry uint16 table indexed by the two bytes (128 KB)
  kAlphaMaxBetaMin,  // 0.9604 max + 0.3978 min (|error| <= 4%, no sqrt)
};

[[nodiscard]] const char* magnitude_name(Magnitude m) noexcept;

// Magnitudes of n samples, relative to full scale (cu8 127.5 counts, cs8
// 128, cs16 32768, cf32 1.0). kLut applies to cu8 only; other formats use
// kExact for it.
void magnitudes(const std::uint8_t* data, std::size_t n, SampleFormat f, Magnitude m, float* out) noexcept;

struct DemodConfig {
  double sample_rate_hz = 2.4e6;
  Magnitude magnitude = Magnitude::kExact;
  int phases = 5;                   // fractional starts tried per sample (1..16)
  // Preamble acceptance (magnitude ratios, not dB).
  double min_signal_ratio = 2.0;    // mean pulse magnitude / noise floor (~6 dB)
  double min_pulse_quiet = 1.2;     // each pulse / mean of the quiet parts
  // Step-1 gate: a window around each preamble pulse must hold this share
  // of what a pulse at min_signal_ratio would put there (lower = slower,
  // and only marginally more sensitive).
  double gate_ratio = 0.9;
  // After a frame, scanning resumes this long before its end (a following
  // reply may start under its tail); below 56 us, so no frame repeats.
  double resume_overlap_us = 16.0;
  // When the half-bit comparison gives no valid frame, try a maximum-
  // likelihood sequence detector that models samples shared between
  // half-bits (step 3b).
  bool sequence_detector = true;
  bool fix_single_bit = false;      // correct one wrong bit in DF11/17/18
  double address_ttl_s = 60.0;      // Address/Parity frames need an address seen this recently
  std::size_t block_samples = 1 << 16;  // most samples per feed() call
};

[[nodiscard]] bool validate(const DemodConfig& cfg, std::string* why = nullptr);

struct DemodFrame {
  std::uint8_t bytes[14] = {};
  std::uint8_t len = 0;             // 7 or 14
  std::uint8_t df = 0;
  std::uint32_t icao = 0;           // for Address/Parity formats: the matched address
  double sample_pos = 0.0;          // preamble start, samples since the stream start
  float signal = 0.0f;              // pulse amplitude (matched-filter estimate), relative to full scale
  float noise = 0.0f;               // noise floor (mean magnitude) at the time
  float snr_db = 0.0f;              // 20 log10(signal / noise)
  float margin = 0.0f;              // mean |first - second half| / signal over the bits
  std::int8_t corrected_bit = -1;   // bit index fixed by fix_single_bit, or -1
};

struct DemodStats {
  std::uint64_t samples = 0;        // fed
  std::uint64_t skipped = 0;        // declared lost with skip()
  std::uint64_t gated = 0;          // positions passing the step-1 gate
  std::uint64_t preambles = 0;      // positions with at least one phase passing step 2
  std::uint64_t frames = 0;         // frames emitted
  std::uint64_t corrected = 0;      // of which one bit was fixed
  std::uint64_t sequence_decoded = 0;  // ... read by the sequence detector (step 3b)
  std::uint64_t bad_crc = 0;        // preambles whose every phase failed the check
  std::uint64_t unknown_address = 0;  // ... of which an Address/Parity frame had an unseen address
  std::uint64_t by_df[32] = {};     // frames emitted per downlink format
};

class Demodulator {
 public:
  explicit Demodulator(const DemodConfig& cfg);
  ~Demodulator();
  Demodulator(const Demodulator&) = delete;
  Demodulator& operator=(const Demodulator&) = delete;

  // Feeds n <= block_samples samples. The returned frames (in time order)
  // stay valid until the next call.
  std::span<const DemodFrame> feed(const std::uint8_t* data, std::size_t n, SampleFormat f);
  // The same with magnitudes already computed (relative to full scale;
  // NaN and infinity count as silence, as in feed()).
  std::span<const DemodFrame> feed_magnitudes(const float* mag, std::size_t n);
  // End of stream: scans the held-back samples (a message cut off by the end
  // is not reported).
  std::span<const DemodFrame> flush();
  // A gap: `n` samples were lost (a dropped SDR block). Scans what is held
  // as at the end of a stream, then continues at absolute sample index
  // pos + n, so frame positions stay on the original sample clock. Returns
  // the frames found before the gap. skip(0) does nothing.
  std::span<const DemodFrame> skip(std::uint64_t n);
  // Absolute index of the next sample feed() takes.
  [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }

  [[nodiscard]] const DemodStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const DemodConfig& config() const noexcept { return cfg_; }
  // Noise floor (mean magnitude of the noise) of the latest complete block.
  [[nodiscard]] float noise_floor() const noexcept { return noise_; }
  // Samples per microsecond.
  [[nodiscard]] double spu() const noexcept { return spu_; }

 private:
  struct Impl;
  void append(std::size_t n);
  void scan(bool final);

  DemodConfig cfg_;
  double spu_ = 2.4;
  std::vector<float> buf_;          // magnitudes, buf_[0] = sample base_
  std::uint64_t base_ = 0;
  std::size_t valid_ = 0;
  std::uint64_t next_ = 0;          // next position to scan (global)
  std::vector<DemodFrame> out_;
  std::size_t n_out_ = 0;
  // Noise floor per 1024-sample block (see noise_for()).
  static constexpr std::size_t kNoiseBlock = 1024;
  static constexpr std::size_t kNoiseWindow = 16;     // samples per window mean
  static constexpr std::size_t kNoiseHistory = 8;     // blocks the floor looks back
  [[nodiscard]] float noise_for(std::uint64_t block, bool final) const noexcept;
  void finish_noise_block() noexcept;
  float noise_ = 0.0f;                                // latest floor (for noise_floor())
  std::vector<float> block_est_;                      // ring by (absolute) block number
  std::vector<std::uint64_t> block_id_;               // which block each slot holds (~0 = none)
  bool have_block_ = false;                           // any block complete yet
  std::uint64_t last_block_ = 0;                      // the latest complete block
  std::uint64_t pos_ = 0;                             // absolute index of the next sample fed
  std::array<float, kNoiseBlock / kNoiseWindow> sub_{};  // window means of this block
  std::size_t n_sub_ = 0;
  std::array<float, kNoiseWindow> win_buf_{};          // a window collected across calls
  std::size_t win_count_ = 0;
  DemodStats stats_;
  std::unique_ptr<Impl> impl_;
};

// Beast binary frame (type '2' or '3') for a demodulated frame: 12 MHz
// timestamp from the sample position, signal byte from the level. Appends to
// `out`; returns the number of bytes added.
std::size_t to_beast(const DemodFrame& f, double sample_rate_hz, std::vector<std::uint8_t>& out);

}  // namespace adsb::demod
