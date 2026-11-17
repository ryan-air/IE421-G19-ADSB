#include "adsb/sim/synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>

#include "adsb/common/rng.hpp"
#include "adsb/modes/cpr.hpp"
#include "adsb/modes/encoder.hpp"

namespace adsb::sim {

namespace {

using modes::Frame;

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kNmPerDegLat = 60.0;
constexpr double kMetersPerNm = 1852.0;
constexpr double kSpeedOfLight = 299'792'458.0;  // m/s

double distance_nm(double lat1, double lon1, double lat2, double lon2) {
  const double dlat = (lat2 - lat1) * kDeg;
  const double dlon = (lon2 - lon1) * kDeg;
  const double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(lat1 * kDeg) * std::cos(lat2 * kDeg) * std::sin(dlon / 2) *
                       std::sin(dlon / 2);
  return 2.0 * 3440.065 * std::asin(std::sqrt(std::min(1.0, h)));
}

double wrap360(double d) {
  d = std::fmod(d, 360.0);
  return d < 0 ? d + 360.0 : d;
}

enum class Kind : std::uint8_t { kPosition, kVelocity, kIdent, kDf11, kDf4, kDf5 };

struct Aircraft {
  AircraftInfo info;
  Rng rng{1};
  // State.
  double lat = 0, lon = 0;
  double alt_ft = 0;
  double gs_kt = 0;
  double track_deg = 0;
  double vr_fpm = 0;
  double turn_rate_dps = 0;  // degrees per second
  bool odd_next = false;
  double t = 0;              // seconds since start, time of `state`
  // Next transmit time per message kind (seconds since start).
  double next[6] = {};
};

void advance(Aircraft& a, double to_t) {
  const double dt = to_t - a.t;
  if (dt <= 0) return;
  // Integrate in <= 1 s steps so turns stay smooth.
  double remaining = dt;
  while (remaining > 0) {
    const double step = std::min(1.0, remaining);
    const double mid_track = a.track_deg + a.turn_rate_dps * step / 2;
    const double d_nm = a.gs_kt * step / 3600.0;
    a.lat += d_nm * std::cos(mid_track * kDeg) / kNmPerDegLat;
    a.lon += d_nm * std::sin(mid_track * kDeg) / (kNmPerDegLat * std::cos(a.lat * kDeg));
    if (a.lon >= 180.0) a.lon -= 360.0;
    if (a.lon < -180.0) a.lon += 360.0;
    a.track_deg = wrap360(a.track_deg + a.turn_rate_dps * step);
    if (!a.info.on_ground) {
      a.alt_ft += a.vr_fpm * step / 60.0;
      // Level off at sensible limits.
      if (a.alt_ft > 41'000) { a.alt_ft = 41'000; a.vr_fpm = 0; }
      if (a.alt_ft < 1'500) { a.alt_ft = 1'500; a.vr_fpm = 0; }
    }
    remaining -= step;
  }
  a.t = to_t;
}

double interval(Rng& rng, Kind k, double surveillance_rate_hz) {
  switch (k) {
    case Kind::kPosition: return rng.uniform(0.4, 0.6);
    case Kind::kVelocity: return rng.uniform(0.4, 0.6);
    case Kind::kIdent: return rng.uniform(4.8, 5.2);
    case Kind::kDf11: return rng.uniform(0.8, 1.2);
    case Kind::kDf4:
    case Kind::kDf5: return surveillance_rate_hz > 0 ? rng.uniform(0.5, 1.5) / surveillance_rate_hz : 1e18;
  }
  return 1e18;
}

std::uint32_t unique_icao(Rng& rng, std::set<std::uint32_t>& used) {
  for (;;) {
    // US civil block (A00001..ADF7C7) for realism.
    const auto icao = static_cast<std::uint32_t>(0xA00001 + rng.below(0xADF7C7 - 0xA00001));
    if (used.insert(icao).second) return icao;
  }
}

void make_callsign(Rng& rng, char out[8]) {
  static constexpr const char* kAirlines[] = {"AAL", "UAL", "DAL", "SWA", "ENY", "SKW", "FDX", "UPS", "JBU", "ASA"};
  std::memset(out, ' ', 8);
  const char* al = kAirlines[rng.below(std::size(kAirlines))];
  std::memcpy(out, al, 3);
  const auto num = 1 + rng.below(9999);
  char digits[8];
  const int n = std::snprintf(digits, sizeof digits, "%llu", static_cast<unsigned long long>(num));
  std::memcpy(out + 3, digits, static_cast<std::size_t>(n));
}

std::uint16_t random_squawk(Rng& rng) {
  for (;;) {
    std::uint16_t s = 0;
    for (int i = 0; i < 4; ++i) s = static_cast<std::uint16_t>((std::uint64_t{s} << 4) | rng.below(8));
    if (s != 0x7500 && s != 0x7600 && s != 0x7700) return s;
  }
}

Aircraft spawn(const SynthConfig& cfg, Rng& master, std::set<std::uint32_t>& used, int index) {
  Aircraft a;
  a.rng = master.fork(static_cast<std::uint64_t>(index) + 1);
  Rng& r = a.rng;
  a.info.icao = unique_icao(r, used);
  make_callsign(r, a.info.callsign);
  a.info.squawk = random_squawk(r);
  a.info.on_ground = r.chance(cfg.ground_fraction);

  if (a.info.on_ground) {
    // Taxiing within ~2 NM of the receiver (an airport).
    const double bearing = r.uniform(0, 360);
    const double dist = r.uniform(0.1, 2.0);
    a.lat = cfg.receiver_lat + dist * std::cos(bearing * kDeg) / kNmPerDegLat;
    a.lon = cfg.receiver_lon + dist * std::sin(bearing * kDeg) / (kNmPerDegLat * std::cos(cfg.receiver_lat * kDeg));
    a.alt_ft = 0;
    a.gs_kt = r.chance(0.3) ? 0.0 : r.uniform(3, 30);
    a.track_deg = r.uniform(0, 360);
    a.info.category = 0xA3;
  } else {
    // Uniform over the coverage disk (sqrt for area-uniform radius).
    const double bearing = r.uniform(0, 360);
    const double dist = cfg.max_range_nm * std::sqrt(r.uniform()) * 0.95;
    a.lat = cfg.receiver_lat + dist * std::cos(bearing * kDeg) / kNmPerDegLat;
    a.lon = cfg.receiver_lon + dist * std::sin(bearing * kDeg) / (kNmPerDegLat * std::cos(cfg.receiver_lat * kDeg));
    a.alt_ft = r.uniform(3'000, 40'000);
    a.gs_kt = r.uniform(140, 520);
    a.track_deg = r.uniform(0, 360);
    if (r.chance(cfg.climb_fraction)) a.vr_fpm = (r.chance(0.5) ? 1 : -1) * r.uniform(500, 3000);
    if (r.chance(cfg.turn_fraction)) a.turn_rate_dps = (r.chance(0.5) ? 1 : -1) * r.uniform(0.5, 3.0);
    static constexpr std::uint8_t kCats[] = {0xA1, 0xA2, 0xA3, 0xA3, 0xA3, 0xA5, 0xA7};
    a.info.category = kCats[r.below(std::size(kCats))];
  }
  // Stagger first transmissions so aircraft are not phase-locked.
  for (int k = 0; k < 6; ++k) {
    a.next[k] = r.uniform(0, interval(r, static_cast<Kind>(k), cfg.surveillance_rate_hz));
  }
  if (!cfg.df11_squitters) a.next[static_cast<int>(Kind::kDf11)] = 1e18;
  if (a.info.on_ground) a.next[static_cast<int>(Kind::kVelocity)] = 1e18;  // surface msg carries speed
  return a;
}

Frame build_frame(const Aircraft& a, Kind k, bool odd) {
  const std::uint32_t icao = a.info.icao;
  const auto alt = static_cast<std::int32_t>(std::lround(a.alt_ft));
  switch (k) {
    case Kind::kPosition: {
      const auto cpr = modes::cpr_encode(a.lat, a.lon, odd, a.info.on_ground);
      if (a.info.on_ground) {
        return modes::encode_surface_position(icao, a.gs_kt, a.gs_kt > 0.5 ? a.track_deg : -1.0, cpr);
      }
      return modes::encode_airborne_position(icao, alt, cpr);
    }
    case Kind::kVelocity: {
      const double e = a.gs_kt * std::sin(a.track_deg * kDeg);
      const double n = a.gs_kt * std::cos(a.track_deg * kDeg);
      return modes::encode_velocity(icao, e, n, static_cast<std::int32_t>(std::lround(a.vr_fpm)));
    }
    case Kind::kIdent:
      return modes::encode_identification(icao, a.info.callsign, a.info.category,
                                          a.info.on_ground ? 4u : 5u);
    case Kind::kDf11:
      return modes::encode_df11(icao, a.info.on_ground ? 4u : 5u);
    case Kind::kDf4:
      return modes::encode_df4(icao, alt, a.info.on_ground ? 1u : 0u);
    case Kind::kDf5:
      return modes::encode_df5(icao, a.info.squawk, a.info.on_ground ? 1u : 0u);
  }
  return {};
}

}  // namespace

record::RecordingInfo recording_info(const SynthConfig& cfg) {
  record::RecordingInfo info;
  info.radio_clock_hz = record::kBeastClockHz;
  info.has_receiver_position = true;
  info.receiver_lat = cfg.receiver_lat;
  info.receiver_lon = cfg.receiver_lon;
  info.receiver_alt_m = cfg.receiver_alt_m;
  info.source = "synthetic";
  return info;
}

SynthOutput generate(const SynthConfig& cfg) {
  SynthOutput out;
  Rng master(cfg.seed);
  Rng reception = master.fork(0xBEEF);
  std::set<std::uint32_t> used;

  std::vector<Aircraft> fleet;
  fleet.reserve(static_cast<std::size_t>(cfg.aircraft));
  for (int i = 0; i < cfg.aircraft; ++i) fleet.push_back(spawn(cfg, master, used, i));
  for (const auto& a : fleet) out.aircraft.push_back(a.info);

  const int lines = std::clamp(cfg.lines, 1, 2);
  // Per-line clock offsets (random, as with two unsynchronized dongles).
  std::uint64_t clock_offset[2];
  for (auto& off : clock_offset) off = reception.below(std::uint64_t{1} << 40);

  for (auto& a : fleet) {
    for (;;) {
      // Next event for this aircraft.
      int kind = 0;
      for (int k = 1; k < 6; ++k) {
        if (a.next[k] < a.next[kind]) kind = k;
      }
      const double t = a.next[kind];
      if (t >= cfg.duration_s) break;
      const auto k = static_cast<Kind>(kind);
      a.next[kind] = t + interval(a.rng, k, cfg.surveillance_rate_hz);
      advance(a, t);

      const bool odd = a.odd_next;
      if (k == Kind::kPosition) a.odd_next = !a.odd_next;
      const Frame f = build_frame(a, k, odd);
      const auto tx_ns = cfg.start_rx_ns + static_cast<std::uint64_t>(std::llround(t * 1e9));
      ++out.stats.transmitted;

      if (k == Kind::kPosition) {
        TruthPoint tp;
        tp.icao = a.info.icao;
        tp.tx_ns = tx_ns;
        tp.lat = a.lat;
        tp.lon = a.lon;
        tp.alt_ft = static_cast<std::int32_t>(std::lround(a.alt_ft));
        tp.ground_speed_kt = a.gs_kt;
        tp.track_deg = a.track_deg;
        tp.vertical_rate_fpm = static_cast<std::int32_t>(std::lround(a.vr_fpm));
        tp.on_ground = a.info.on_ground;
        out.truth.push_back(tp);
      }

      const double range = distance_nm(cfg.receiver_lat, cfg.receiver_lon, a.lat, a.lon);
      const double prop_ns = range * kMetersPerNm / kSpeedOfLight * 1e9;
      for (int line = 0; line < lines; ++line) {
        // Reception decision for this line.
        double p_loss = 1.0;
        if (range <= cfg.max_range_nm) {
          const double frac = range / cfg.max_range_nm;
          p_loss = std::min(1.0, cfg.base_loss + cfg.range_loss * frac * frac);
        }
        if (a.rng.chance(p_loss)) {
          ++out.stats.lost;
          continue;
        }
        record::Record r;
        r.line = static_cast<std::uint8_t>(line);
        r.type = f.len == 14 ? '3' : '2';
        r.len = f.len;
        std::memcpy(r.payload, f.bytes, f.len);
        if (a.rng.chance(cfg.corrupt_rate)) {
          const auto bit = a.rng.below(f.len * 8u);
          r.payload[bit / 8] ^= static_cast<std::uint8_t>(0x80u >> (bit % 8));
          ++out.stats.corrupted;
        }
        const double latency_us = cfg.rx_latency_us + (line == 1 ? cfg.line_b_extra_latency_us : 0.0) +
                                   a.rng.uniform(0, cfg.rx_jitter_us);
        r.rx_ns = tx_ns + static_cast<std::uint64_t>(std::llround(prop_ns + latency_us * 1e3));
        // Radio clock: 12 MHz ticks of arrival time, with offset and drift.
        const double arrival_s = (static_cast<double>(tx_ns - cfg.start_rx_ns) + prop_ns) * 1e-9;
        const double ticks = arrival_s * 12e6 * (1.0 + cfg.line_drift_ppm[line] * 1e-6);
        r.radio_ts = (clock_offset[line] + static_cast<std::uint64_t>(std::llround(ticks))) &
                     0xFFFFFFFFFFFFULL;
        const double sig = 255.0 * std::clamp(1.0 - range / cfg.max_range_nm, 0.05, 1.0);
        r.signal = static_cast<std::uint8_t>(sig);
        out.records.push_back(r);
        ++out.stats.received;
      }
    }
  }

  // Sort into arrival order. stable_sort keeps generation order for exact
  // ties, so the result is deterministic on every standard library.
  std::stable_sort(out.records.begin(), out.records.end(),
                   [](const record::Record& x, const record::Record& y) { return x.rx_ns < y.rx_ns; });
  std::stable_sort(out.truth.begin(), out.truth.end(),
                   [](const TruthPoint& x, const TruthPoint& y) { return x.tx_ns < y.tx_ns; });
  return out;
}

}  // namespace adsb::sim
