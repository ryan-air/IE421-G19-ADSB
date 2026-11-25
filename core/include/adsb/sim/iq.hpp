#pragma once

// Synthetic IQ scenario generator (README_UPDATES.md §5, §22.4).
//
// Turns adsb-synth traffic (synth.hpp, with keep_transmissions) into the
// complex baseband samples a 1090 MHz receiver would record, with per-message
// ground truth. Signal chain, per transmission:
//
//   waveform     Mode S pulse-position modulation: 4-pulse preamble (0, 1,
//                3.5, 4.5 us), then 56 or 112 bits of 1 us; trapezoidal
//                pulses with finite rise and fall times; carrier phase
//                continuous across the message
//   transmitter  per-aircraft power (125-500 W) and carrier offset from
//                1090 MHz, linear drift over time, per-message jitter
//   geometry     WGS-84 positions, free-space path loss, propagation delay,
//                receive-antenna gain by elevation (vertical dipole)
//   Doppler      from the range rate (aircraft velocity along the line of
//                sight): f_rx = f_tx (1 - rdot / c)
//   atmosphere   effective-Earth (k-factor, default 4/3) radio horizon;
//                beyond it a diffraction loss per km. Gaseous absorption and
//                rain are below 0.01 dB/km at 1 GHz and are left out
//   multipath    two-ray ground reflection over the same effective Earth
//                (specular point, path difference, divergence factor;
//                |Gamma| with phase pi) and optional fixed specular
//                reflectors (buildings, hills) that add delayed echoes
//   collisions   overlapping replies simply add
//   Mode A/C     optional (IqConfig::modeac_rate_hz): replies to simulated
//                SSR radars (rotating beams, bursts of interrogations),
//                identity or Gillham altitude, 0.45 us pulses 1.45 us apart
//                (modes/mode_ac.hpp); same transmitter, channel and receiver
//                as the Mode S messages
//   receiver     LO and sample-clock error from one crystal (ppm, as on an
//                RTL-SDR), thermal noise from the noise figure, DC offset,
//                IQ gain/phase imbalance, then the ADC: clipping and
//                quantization to cu8 / cs8 / cs16 (cf32 is not quantized)
//
// Each output sample is the average of `oversample` points over its sample
// period (a boxcar: the integrate-and-dump of a decimating receiver), so
// fractional arrival times and edges are represented exactly.
//
// Deterministic: a config gives the same bytes on every platform and for any
// thread count (portable_math.hpp; noise is seeded per fixed-size chunk).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adsb/sim/synth.hpp"

namespace adsb::sim {

inline constexpr double kModeSCarrierHz = 1'090'000'000.0;

enum class IqFormat : std::uint8_t { kCu8, kCs8, kCs16, kCf32 };

[[nodiscard]] const char* iq_format_name(IqFormat f) noexcept;   // "cu8", ...
[[nodiscard]] const char* sigmf_datatype(IqFormat f) noexcept;   // "cu8", "ci8", "ci16_le", "cf32_le"
[[nodiscard]] std::size_t iq_bytes_per_sample(IqFormat f) noexcept;  // per complex sample
[[nodiscard]] std::optional<IqFormat> parse_iq_format(std::string_view s) noexcept;

enum class AntennaPattern : std::uint8_t { kOmni, kDipole };

// A fixed specular reflector. The echo travels transmitter -> reflector ->
// receiver; its power is the free-space budget over that total length minus
// `loss_db`.
struct Reflector {
  double lat = 0.0;
  double lon = 0.0;
  double alt_m = 0.0;     // above mean sea level
  double loss_db = 10.0;
};

struct IqConfig {
  std::uint64_t seed = 1;          // noise, carrier phases, transmitter draws

  // Sampling.
  double sample_rate_hz = 2.4e6;
  double center_hz = kModeSCarrierHz;
  IqFormat format = IqFormat::kCu8;
  int oversample = 8;              // points averaged per output sample (1..64)

  // Receiver. The site is SynthConfig's receiver; its altitude is the ground.
  double antenna_height_m = 10.0;
  AntennaPattern antenna = AntennaPattern::kDipole;
  double cable_loss_db = 1.0;
  double noise_figure_db = 5.0;
  double full_scale_dbm = -70.0;   // antenna-port power that reaches amplitude 1.0
  bool noise = true;
  double rx_ppm = 0.0;             // LO and sample-clock error (same crystal)
  // LO drift (a receiver warming up): the LO error grows by this many ppm
  // per second from rx_ppm at t = 0. Only the LO: the sample clock keeps
  // rx_ppm (its drift would change frequencies by < 1 Hz but move every
  // sample position, which the truth annotations assume fixed).
  double lo_drift_ppm_per_s = 0.0;
  double dc_i = 0.0;               // DC offset, fraction of full scale
  double dc_q = 0.0;
  double iq_gain_imbalance = 0.0;  // Q gain is (1 + this)
  double iq_phase_deg = 0.0;       // Q leaks this much of I

  // Transmitters (drawn per aircraft from seed and ICAO address).
  double tx_power_min_dbm = 51.0;  // 125 W
  double tx_power_max_dbm = 57.0;  // 500 W
  double tx_offset_sd_hz = 100e3;  // Gaussian, clamped to the +-1 MHz tolerance
  double tx_drift_sd_hz_per_s = 0.5;
  double tx_jitter_sd_hz = 20.0;   // per message
  double rise_us = 0.08;           // pulse edges (10-90%-ish; linear ramps)
  double fall_us = 0.12;

  // Channel.
  bool doppler = true;
  double k_factor = 4.0 / 3.0;     // effective Earth radius factor
  double diffraction_db_per_km = 1.0;  // beyond the radio horizon
  double ground_reflection = 0.3;  // |Gamma| of the two-ray ground bounce; 0 = off
  std::vector<Reflector> reflectors;

  // Noise-only seconds after the traffic ends (readsb drops about the last
  // second of an input file, so scoring runs add a tail).
  double tail_s = 0.0;

  // Messages weaker than this (direct-path SNR) are neither rendered nor
  // annotated: they would be indistinguishable from noise.
  double min_snr_db = -15.0;

  // Mode A/C replies (off when modeac_rate_hz is 0; then nothing below has
  // any effect and the output is that of a Mode S-only scenario). Every
  // airborne aircraft answers `modeac_radars` rotating SSR radars: each
  // radar (rotation period 4.5-12 s, beam modeac_beam_deg wide; 360 = an
  // interrogator that is always on it) interrogates an aircraft in a burst
  // while its beam passes, at a PRF chosen so that
  // the mean reply rate per aircraft is modeac_rate_hz. Each interrogation
  // asks for Mode C with probability modeac_mode_c_share (else Mode A) and
  // is answered with probability modeac_reply_efficiency. A share of the
  // aircraft (modeac_spi_share) has ident pressed: their Mode A replies
  // carry SPI. Aircraft on the ground do not reply (squat switch). Mode A/C
  // draws use their own random streams: the Mode S messages, their noise
  // and their samples are unchanged by them (only collision flags can
  // change, where a Mode A/C reply overlaps).
  double modeac_rate_hz = 0.0;
  int modeac_radars = 3;
  double modeac_beam_deg = 3.0;
  double modeac_mode_c_share = 0.5;
  double modeac_reply_efficiency = 0.9;
  double modeac_spi_share = 0.0;
};

// Checks every field is finite and in a physical range (the renderer assumes
// it: out-of-range values can give infinities). On failure *why says which.
[[nodiscard]] bool validate_iq_config(const IqConfig& cfg, std::string* why = nullptr);

// Receiver site and emitter state for one link (also used by tests).
struct Site {
  double lat = 0.0;
  double lon = 0.0;
  double ground_alt_m = 0.0;
  double antenna_height_m = 10.0;
};

struct Emitter {
  double lat = 0.0;
  double lon = 0.0;
  double alt_m = 0.0;              // above mean sea level
  double vel_east_mps = 0.0;
  double vel_north_mps = 0.0;
  double vel_up_mps = 0.0;
  double power_dbm = 54.0;
  double carrier_offset_hz = 0.0;  // transmitted carrier minus center_hz
};

inline constexpr int kMaxIqPaths = 2 + 6;   // direct, ground bounce, reflectors

struct IqPath {
  double delay_s = 0.0;            // after the direct arrival
  double amplitude = 0.0;          // relative to ADC full scale
  double phase_rad = 0.0;          // relative to the direct path
  double freq_hz = 0.0;            // baseband frequency of this path
};

struct LinkResult {
  double range_m = 0.0;            // slant range, direct path
  double ground_distance_m = 0.0;
  double range_rate_mps = 0.0;     // > 0 receding
  double elevation_deg = 0.0;      // at the receiver, with the k-factor bending
  double horizon_m = 0.0;          // radio horizon distance for this pair
  double path_loss_db = 0.0;       // free space + diffraction
  double antenna_gain_db = 0.0;
  double power_dbm = 0.0;          // direct path at the antenna port, after cable
  double doppler_hz = 0.0;
  double baseband_hz = 0.0;        // direct path: offset + Doppler - LO error
  int n_paths = 0;
  IqPath paths[kMaxIqPaths];
};

[[nodiscard]] LinkResult compute_link(const Site& site, const Emitter& e, const IqConfig& cfg);

// Per-aircraft transmitter (truth for the frequency-stability study, §22.5).
struct IqTransmitter {
  std::uint32_t icao = 0;
  std::uint8_t category = 0;
  double power_dbm = 0.0;
  double offset_hz = 0.0;          // at t = 0
  double drift_hz_per_s = 0.0;
};

// One rendered message and its truth. A Mode A/C reply has len 2, bytes =
// its code word big-endian (modes/mode_ac.hpp), icao = the aircraft that
// sent it and mode_ac 'A' or 'C' (0 for Mode S). sample_pos is F1's leading
// edge.
struct IqMessage {
  std::uint64_t tx_ns = 0;
  std::uint32_t icao = 0;
  std::uint8_t len = 0;
  std::uint8_t bytes[14] = {};
  char mode_ac = 0;
  double lat = 0.0, lon = 0.0, alt_ft = 0.0;
  double arrival_s = 0.0;          // direct path, true time since the start
  double sample_pos = 0.0;         // direct-path preamble start, in recording samples
  std::int64_t first_sample = 0;   // span touched by any path, inclusive
  std::int64_t last_sample = 0;
  double snr_db = 0.0;             // direct-path power over noise in the full bandwidth
  double tx_offset_hz = 0.0;       // transmitter carrier - center (incl. drift, jitter)
  double observed_hz = 0.0;        // frequency as measured from the recording's samples
  double phase0 = 0.0;             // carrier phase at the preamble start
  LinkResult link;
  bool collided = false;           // overlaps another rendered message
  double interferer_db = -999.0;   // strongest overlapping message relative to this one
};

struct IqPlanStats {
  std::uint64_t transmissions = 0;
  std::uint64_t rendered = 0;
  std::uint64_t too_weak = 0;      // below min_snr_db
  std::uint64_t beyond_horizon = 0;  // rendered or not
  std::uint64_t collided = 0;
  std::uint64_t modeac_replies = 0;  // Mode A/C replies rendered (included in rendered)
  std::uint64_t modeac_too_weak = 0;  // ... below min_snr_db (included in too_weak)
};

struct IqPlan {
  std::vector<IqMessage> messages;       // sorted by first_sample
  std::vector<IqTransmitter> transmitters;  // one per aircraft
  std::uint64_t total_samples = 0;
  double noise_dbm = 0.0;                // thermal noise over the sample bandwidth
  double noise_sigma = 0.0;              // per component, relative to full scale
  std::int64_t max_span_samples = 0;     // longest message span (for chunk lookups)
  IqPlanStats stats;
};

// Plans the recording: `traffic` must come from generate() with
// keep_transmissions set, and cfg must pass validate_iq_config(). The
// recording lasts the traffic's duration plus the tail, extended so that
// every message (propagation delay and echoes included) ends inside it.
[[nodiscard]] IqPlan plan_iq(const SynthConfig& synth, const SynthOutput& traffic, const IqConfig& cfg);

// Rendering works on fixed chunks so the noise (seeded per chunk) and thus
// the output do not depend on how the caller splits the work.
inline constexpr std::size_t kIqChunkSamples = 1 << 16;

// Renders chunk `chunk` (samples [chunk * kIqChunkSamples, ...)) as
// interleaved I/Q floats relative to full scale, analog impairments
// applied, not yet quantized. `out` holds 2 * kIqChunkSamples floats;
// returns the number of samples in the chunk (fewer for the last one).
// Thread-safe: plan and config are only read.
std::size_t render_iq_chunk(const IqPlan& plan, const IqConfig& cfg, std::uint64_t chunk, float* out);

// ADC: clips to full scale and quantizes n samples into `dst`
// (n * iq_bytes_per_sample(format) bytes). Returns the number of samples
// with a clipped component.
std::uint64_t quantize_iq(const float* iq, std::size_t n, IqFormat format, std::uint8_t* dst);

// SigMF metadata (sigmf.org, v1.0.0) for the plan: global (datatype, rate,
// receiver, the configuration, every transmitter), one capture at
// center_hz, one annotation per rendered message with its truth.
[[nodiscard]] std::string sigmf_meta(const SynthConfig& synth, const IqConfig& cfg, const IqPlan& plan);

}  // namespace adsb::sim
