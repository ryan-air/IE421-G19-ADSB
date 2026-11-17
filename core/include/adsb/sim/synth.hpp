#pragma once

// Synthetic ADS-B traffic generator (README_UPDATES.md §11.4).
//
// Simulates aircraft flying realistic paths around a receiver and produces the
// Mode S frames a receiver would record, with:
//  * DO-260B-like transmit schedules: airborne position and velocity every
//    0.4-0.6 s (even/odd CPR alternating), identification every 4.8-5.2 s,
//    DF11 acquisition squitters ~1 Hz, and optional DF4/DF5 interrogation
//    replies; surface position for aircraft on the ground;
//  * a reception model: range-dependent loss, independent per receiver line
//    (A/B), bit corruption, propagation delay, and per-line latency jitter;
//  * per-line 12 MHz radio clocks with offset and drift (groundwork for MLAT);
//  * ground truth: the true state at every transmitted position, so tests can
//    measure decoder and track-state error exactly.
//
// Fully deterministic for a given config (portable RNG, see rng.hpp). Output
// is sorted by receive time, ready to write as an .adsbrec recording.

#include <cstdint>
#include <string>
#include <vector>

#include "adsb/record/recording.hpp"

namespace adsb::sim {

struct SynthConfig {
  std::uint64_t seed = 1;

  // Receiver (default: Willard Airport, Champaign IL).
  double receiver_lat = 40.0392;
  double receiver_lon = -88.2781;
  std::int32_t receiver_alt_m = 230;

  int aircraft = 50;
  double ground_fraction = 0.06;    // share of aircraft taxiing near the receiver
  double duration_s = 60.0;
  double max_range_nm = 150.0;      // nothing beyond this is received

  // Aircraft behaviour.
  double turn_fraction = 0.3;       // aircraft in a constant-rate turn
  double climb_fraction = 0.35;     // aircraft climbing or descending

  // Extra message types.
  bool df11_squitters = true;       // ~1 Hz acquisition squitters
  double surveillance_rate_hz = 0.3;  // DF4/DF5 replies per aircraft (0 = none)

  // Reception model, applied independently per line.
  int lines = 1;                    // 1 or 2 receiver lines (A/B)
  double base_loss = 0.05;          // loss probability at zero range
  double range_loss = 0.55;         // additional loss at max range (quadratic in range)
  double corrupt_rate = 0.003;      // share of received frames with one flipped bit
  double rx_latency_us = 1500.0;    // fixed receiver + network latency (line A)
  double rx_jitter_us = 300.0;      // uniform jitter added to each frame
  double line_b_extra_latency_us = 250.0;  // line B is a little slower

  // Radio clocks (12 MHz, Beast-style): per-line offset and drift.
  double line_drift_ppm[2] = {0.0, 18.0};

  // Event-time origin for rx_ns (monotonic clock domain).
  std::uint64_t start_rx_ns = 1'000'000'000'000ULL;
};

// True aircraft state at the moment a position frame was transmitted.
struct TruthPoint {
  std::uint32_t icao = 0;
  std::uint64_t tx_ns = 0;        // transmit time (same clock domain as rx_ns)
  double lat = 0.0;
  double lon = 0.0;
  std::int32_t alt_ft = 0;
  double ground_speed_kt = 0.0;
  double track_deg = 0.0;
  std::int32_t vertical_rate_fpm = 0;
  bool on_ground = false;
};

struct AircraftInfo {
  std::uint32_t icao = 0;
  char callsign[8] = {};
  std::uint8_t category = 0;
  std::uint16_t squawk = 0;
  bool on_ground = false;
};

struct SynthStats {
  std::uint64_t transmitted = 0;
  std::uint64_t received = 0;      // summed over lines
  std::uint64_t lost = 0;
  std::uint64_t corrupted = 0;
};

struct SynthOutput {
  std::vector<record::Record> records;  // sorted by rx_ns
  std::vector<TruthPoint> truth;        // sorted by tx_ns
  std::vector<AircraftInfo> aircraft;
  SynthStats stats;
};

[[nodiscard]] SynthOutput generate(const SynthConfig& config);

// Recording metadata matching a config (receiver position, source "synthetic").
[[nodiscard]] record::RecordingInfo recording_info(const SynthConfig& config);

}  // namespace adsb::sim
