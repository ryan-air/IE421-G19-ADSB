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

  // Also return every transmission before the reception model, with the
  // aircraft's true state (SynthOutput::transmissions). The IQ generator
  // (iq.hpp) renders these. Off by default: it costs memory and does not
  // change anything else (no extra random numbers are drawn).
  bool keep_transmissions = false;

  // Avionics variety (off by default; when off, no extra random numbers are
  // drawn and the output is unchanged). Each aircraft gets an ADS-B version
  // (0, 1 or 2), a position type code with NIC supplement bits and the NIC
  // they mean (AircraftInfo::nic, from a table of DO-260B rows), and sends
  // operational status (TC 31) every 2.4-2.6 s (version 1 and 2). A share
  // of airborne aircraft sends airspeed and heading (velocity subtype 3)
  // instead of ground speed.
  bool avionics = false;
  double airspeed_fraction = 0.3;
};

// One transmitted frame with the transmitter's true state at that moment.
struct Transmission {
  std::uint64_t tx_ns = 0;        // same clock domain as rx_ns
  std::uint32_t icao = 0;
  std::uint8_t len = 0;           // 7 or 14
  std::uint8_t bytes[14] = {};
  double lat = 0.0;
  double lon = 0.0;
  double alt_ft = 0.0;            // barometric = geometric here (no pressure model)
  double ground_speed_kt = 0.0;
  double track_deg = 0.0;
  double vertical_rate_fpm = 0.0;
  bool on_ground = false;
};

// True aircraft state at the moment a position frame was transmitted.
// heading_deg / airspeed_kt are what the aircraft would report in velocity
// subtype 3 (only meaningful with AircraftInfo::airspeed_velocity).
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
  double heading_deg = 0.0;
  double airspeed_kt = 0.0;
};

struct AircraftInfo {
  std::uint32_t icao = 0;
  char callsign[8] = {};
  std::uint8_t category = 0;
  std::uint16_t squawk = 0;
  bool on_ground = false;
  // With SynthConfig::avionics (otherwise: version 0, TC 11 / TC 7, NIC 8).
  std::uint8_t adsb_version = 0;
  std::uint8_t position_tc = 11;
  bool nic_a = false;
  bool nic_b = false;
  bool nic_c = false;
  std::uint8_t nic = 8;              // what the position messages mean once TC 31 is known
  bool heading_true_north = false;   // HRD (version >= 1); version 0: magnetic
  bool airspeed_velocity = false;    // sends velocity subtype 3 (airspeed + heading)
  bool airspeed_true = false;        // TAS (else IAS)
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
  std::vector<Transmission> transmissions;  // sorted by tx_ns; only with keep_transmissions
  SynthStats stats;
};

[[nodiscard]] SynthOutput generate(const SynthConfig& config);

// Recording metadata matching a config (receiver position, source "synthetic").
[[nodiscard]] record::RecordingInfo recording_info(const SynthConfig& config);

}  // namespace adsb::sim
