#include "adsb/sim/iq.hpp"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "adsb/common/portable_math.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/modes/altitude.hpp"
#include "adsb/modes/mode_ac.hpp"

namespace adsb::sim {

namespace {

constexpr double kC = 299'792'458.0;
constexpr double kDeg = pm::kPi / 180.0;
constexpr double kMeanEarthRadius = 6'371'000.0;
constexpr double kWgs84A = 6'378'137.0;
constexpr double kWgs84F = 1.0 / 298.257223563;
constexpr double kWgs84E2 = kWgs84F * (2.0 - kWgs84F);
constexpr double kKtToMps = 1852.0 / 3600.0;
constexpr double kFpmToMps = 0.3048 / 60.0;
constexpr double kFtToM = 0.3048;
constexpr double kGroundAntennaM = 3.0;  // transponder antenna of a taxiing aircraft

struct Vec3 {
  double x = 0, y = 0, z = 0;
};
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double norm(Vec3 a) { return std::sqrt(dot(a, a)); }

Vec3 ecef(double lat_deg, double lon_deg, double alt_m) {
  double sl, cl, so, co;
  pm::sincos(lat_deg * kDeg, sl, cl);
  pm::sincos(lon_deg * kDeg, so, co);
  const double n = kWgs84A / std::sqrt(1.0 - kWgs84E2 * sl * sl);
  return {(n + alt_m) * cl * co, (n + alt_m) * cl * so, (n * (1.0 - kWgs84E2) + alt_m) * sl};
}

// East/north/up velocity at (lat, lon) in ECEF.
Vec3 enu_to_ecef(double lat_deg, double lon_deg, double e, double n, double u) {
  double sl, cl, so, co;
  pm::sincos(lat_deg * kDeg, sl, cl);
  pm::sincos(lon_deg * kDeg, so, co);
  return {-so * e - sl * co * n + cl * co * u, co * e - sl * so * n + cl * so * u, cl * n + sl * u};
}

double ground_distance(double lat1, double lon1, double lat2, double lon2) {
  const double s1 = pm::sin((lat2 - lat1) * kDeg / 2);
  const double s2 = pm::sin((lon2 - lon1) * kDeg / 2);
  const double h = std::min(1.0, s1 * s1 + pm::cos(lat1 * kDeg) * pm::cos(lat2 * kDeg) * s2 * s2);
  return 2.0 * kMeanEarthRadius * pm::atan2(std::sqrt(h), std::sqrt(1.0 - h));
}

// Vertical half-wave dipole: 2.15 dBi at the horizon, null overhead.
double antenna_gain_db(AntennaPattern p, double elevation_rad) {
  if (p == AntennaPattern::kOmni) return 0.0;
  const double c = pm::cos(elevation_rad);
  if (c < 1e-3) return -40.0;
  const double f = pm::cos(pm::kPi / 2 * pm::sin(elevation_rad)) / c;
  return std::max(-40.0, pm::power_to_db(1.64 * f * f));
}

double fspl_db(double range_m, double freq_hz) {
  return 20.0 * pm::log10(4.0 * pm::kPi * std::max(range_m, 1.0) * freq_hz / kC);
}

double frac(double x) { return x - std::floor(x); }

double gaussian(Rng& r) {
  // Box-Muller with portable functions.
  const double u1 = 1.0 - r.uniform();  // (0, 1]
  const double u2 = r.uniform();
  return std::sqrt(-2.0 * pm::log(u1)) * pm::cos(pm::kTwoPi * u2);
}

constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ULL;

std::uint64_t mix(std::uint64_t a, std::uint64_t b) {
  std::uint64_t z = a ^ (b * kGolden);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

struct Interval {
  double a, b;  // seconds after the preamble start
};

// Merged pulse intervals of a Mode S reply: preamble, then PPM bits. Built
// in integer half-microseconds so adjacent half-bits merge exactly.
int pulse_intervals(const std::uint8_t* bytes, int len, Interval* out) {
  int n = 0;
  int ia[4 + 112], ib[4 + 112];
  auto add = [&](int a, int b) {
    if (n > 0 && ib[n - 1] == a) {
      ib[n - 1] = b;  // adjacent half-bits form one wide pulse
    } else {
      ia[n] = a;
      ib[n] = b;
      ++n;
    }
  };
  add(0, 1);
  add(2, 3);
  add(7, 8);
  add(9, 10);
  const int bits = len * 8;
  for (int i = 0; i < bits; ++i) {
    const bool one = (bytes[i / 8] >> (7 - i % 8)) & 1;
    const int h = 16 + 2 * i;
    if (one) add(h, h + 1);
    else add(h + 1, h + 2);
  }
  for (int i = 0; i < n; ++i) out[i] = {ia[i] * 0.5e-6, ib[i] * 0.5e-6};
  return n;
}

double message_duration_s(const IqMessage& m) {
  if (m.mode_ac) return modes::mode_ac_duration_us(static_cast<std::uint16_t>(m.bytes[0] << 8 | m.bytes[1])) * 1e-6;
  return (8.0 + m.len * 8.0) * 1e-6;
}

// Pulses of a Mode A/C reply (code word in bytes[0..1]).
int mode_ac_intervals(const std::uint8_t* bytes, Interval* out) {
  const std::uint32_t mask = modes::mode_ac_pulses(static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]));
  int n = 0;
  for (int k = 0; k < modes::kModeAcSlots; ++k) {
    if (mask >> k & 1u) {
      const double a = k * modes::kModeAcSlotUs * 1e-6;
      out[n++] = {a, a + modes::kModeAcPulseUs * 1e-6};
    }
  }
  return n;
}

struct Fmt {
  std::string s;
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    va_list again;
    va_copy(again, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
      const std::size_t at = s.size();
      s.resize(at + static_cast<std::size_t>(n) + 1);
      std::vsnprintf(s.data() + at, static_cast<std::size_t>(n) + 1, fmt, again);
      s.resize(at + static_cast<std::size_t>(n));
    }
    va_end(again);
  }
};

// Smooth-Earth (radius re) specular reflection between heights h_r and h_t
// above ground, ground distance d. The reflection point d1 (from the
// receiver) makes equal grazing angles: h_r' / d1 = h_t' / d2 with effective
// heights h' = h - d_i^2 / (2 re). g(d1) = h_r' d2 - h_t' d1 is positive at
// d1 = 0 and negative at d1 = d, so bisection finds it (a fixed-point
// iteration oscillates and diverges at long range). Returns false when the
// ground hides the reflection point.
struct Bounce {
  double delta_m;      // reflected minus direct path length
  double divergence;   // spreading of the ray bundle off a convex surface
  double grazing_rad;
};
bool spherical_bounce(double h_r, double h_t, double d, double re, Bounce& out) {
  if (d < 1.0) return false;
  auto g = [&](double x) {
    const double y = d - x;
    return (h_r - x * x / (2.0 * re)) * y - (h_t - y * y / (2.0 * re)) * x;
  };
  double lo = 0.0, hi = d;
  for (int it = 0; it < 80; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (g(mid) > 0) lo = mid;
    else hi = mid;
  }
  const double d1 = 0.5 * (lo + hi);
  const double d2 = d - d1;
  const double hr = h_r - d1 * d1 / (2.0 * re);
  const double ht = h_t - d2 * d2 / (2.0 * re);
  if (hr <= 0.0 || ht <= 0.0) return false;
  // Image method over the tangent plane at the reflection point, written so
  // it does not cancel at long range: R2 - R1 = 4 h_r' h_t' / (R1 + R2).
  const double r1 = std::sqrt(d * d + (ht - hr) * (ht - hr));
  const double r2 = std::sqrt(d * d + (ht + hr) * (ht + hr));
  out.delta_m = 4.0 * hr * ht / (r1 + r2);
  out.divergence = 1.0 / std::sqrt(1.0 + 2.0 * d1 * d2 / (re * (hr + ht)));
  out.grazing_rad = pm::atan2(hr + ht, d);
  return true;
}

struct ModeAcReply {
  Transmission tr;   // len 2, bytes = code word; state of the aircraft at tx_ns
  char mode = 'A';
};

// Mode A/C replies to simulated SSR radars (IqConfig::modeac_*). Radar r
// rotates with period P_r (4.5-12 s) and a beam b degrees wide, so it
// illuminates an aircraft for P_r b / 360 s once per turn, at a phase that
// depends on the aircraft's bearing (drawn per aircraft and radar). While
// the beam is on it, interrogations come at the radar's PRF (with a 2%
// stagger), chosen so that the mean reply rate is modeac_rate_hz per
// aircraft over all radars. The aircraft's state at a reply is that of its
// nearest Mode S transmission, dead-reckoned by its velocity.
std::vector<ModeAcReply> plan_mode_ac(const SynthConfig& synth, const SynthOutput& traffic, const IqConfig& cfg) {
  std::vector<ModeAcReply> out;
  const int radars = std::max(1, cfg.modeac_radars);
  const double eff = cfg.modeac_reply_efficiency;
  const double beam = cfg.modeac_beam_deg;
  const double prf = cfg.modeac_rate_hz * 360.0 / (radars * beam * eff);
  const double duration = std::max(0.0, synth.duration_s);

  std::unordered_map<std::uint32_t, std::vector<std::size_t>> by_icao;
  for (std::size_t i = 0; i < traffic.transmissions.size(); ++i) by_icao[traffic.transmissions[i].icao].push_back(i);
  for (auto& [icao, v] : by_icao) {
    std::stable_sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) {
      return traffic.transmissions[a].tx_ns < traffic.transmissions[b].tx_ns;
    });
  }
  std::vector<double> period(static_cast<std::size_t>(radars));
  for (int r = 0; r < radars; ++r) {
    Rng rr(mix(cfg.seed, std::uint64_t{0xAC0000000000} + static_cast<std::uint64_t>(r)));
    period[static_cast<std::size_t>(r)] = rr.uniform(4.5, 12.0);
  }

  for (const AircraftInfo& a : traffic.aircraft) {
    const auto found = by_icao.find(a.icao);
    if (found == by_icao.end()) continue;
    const std::vector<std::size_t>& txs = found->second;
    Rng ar(mix(cfg.seed, std::uint64_t{0xAC5D000000000000} + a.icao));
    const bool spi = ar.chance(cfg.modeac_spi_share);
    for (int r = 0; r < radars; ++r) {
      Rng rr(mix(cfg.seed, mix(std::uint64_t{0xACAC000000000000} + static_cast<std::uint64_t>(r), a.icao)));
      const double p = period[static_cast<std::size_t>(r)];
      const double dwell = p * beam / 360.0;
      const double phase = rr.uniform(0.0, p);
      for (double scan = phase - p; scan < duration; scan += p) {
        for (int k = 0;; ++k) {
          if (k / prf >= dwell) break;
          const double t = scan + (k + rr.uniform(-0.02, 0.02)) / prf;
          const bool answered = rr.chance(eff);
          const bool mode_c = rr.chance(cfg.modeac_mode_c_share);
          if (!answered || t < 0.0 || t >= duration) continue;
          const auto tx_ns = synth.start_rx_ns + static_cast<std::uint64_t>(std::llround(t * 1e9));
          // Nearest Mode S transmission of the aircraft.
          const auto pos = std::lower_bound(txs.begin(), txs.end(), tx_ns, [&](std::size_t i, std::uint64_t v) {
            return traffic.transmissions[i].tx_ns < v;
          });
          std::size_t near = pos == txs.end() ? txs.back() : *pos;
          if (pos != txs.begin() && pos != txs.end()) {
            const std::size_t prev = *(pos - 1);
            if (tx_ns - traffic.transmissions[prev].tx_ns < traffic.transmissions[near].tx_ns - tx_ns) near = prev;
          }
          const Transmission& ref = traffic.transmissions[near];
          if (ref.on_ground) continue;  // squat switch: no Mode A/C on the ground
          const double dt = (static_cast<double>(tx_ns) - static_cast<double>(ref.tx_ns)) * 1e-9;
          if (dt > 10.0 || dt < -10.0) continue;
          ModeAcReply rep;
          rep.mode = mode_c ? 'C' : 'A';
          Transmission& tr = rep.tr;
          tr = ref;
          tr.tx_ns = tx_ns;
          double st, ct;
          pm::sincos(ref.track_deg * kDeg, st, ct);
          const double v = ref.ground_speed_kt * kKtToMps * dt;
          tr.lat = ref.lat + v * ct / kMeanEarthRadius / kDeg;
          tr.lon = ref.lon + v * st / (kMeanEarthRadius * std::max(1e-6, pm::cos(ref.lat * kDeg))) / kDeg;
          tr.alt_ft = ref.alt_ft + ref.vertical_rate_fpm * dt / 60.0;
          std::uint16_t code = 0;
          if (mode_c) {
            // Pressure altitude to the nearest 100 ft; none (only the
            // framing pulses) out of the code's range.
            const double hundreds = std::floor((tr.alt_ft + 50.0) / 100.0);
            code = hundreds >= -12.0 && hundreds <= 1267.0
                       ? modes::feet_to_gillham(static_cast<std::int32_t>(hundreds) * 100).value_or(0)
                       : 0;
          } else {
            code = static_cast<std::uint16_t>((a.squawk & 0x7777u) | (spi ? modes::kModeAcSpiBit : 0u));
          }
          tr.len = 2;
          std::memset(tr.bytes, 0, sizeof tr.bytes);
          tr.bytes[0] = static_cast<std::uint8_t>(code >> 8);
          tr.bytes[1] = static_cast<std::uint8_t>(code & 0xFF);
          out.push_back(rep);
        }
      }
    }
  }
  return out;
}

}  // namespace

const char* iq_format_name(IqFormat f) noexcept {
  switch (f) {
    case IqFormat::kCu8: return "cu8";
    case IqFormat::kCs8: return "cs8";
    case IqFormat::kCs16: return "cs16";
    case IqFormat::kCf32: return "cf32";
  }
  return "?";
}

const char* sigmf_datatype(IqFormat f) noexcept {
  switch (f) {
    case IqFormat::kCu8: return "cu8";
    case IqFormat::kCs8: return "ci8";
    case IqFormat::kCs16: return "ci16_le";
    case IqFormat::kCf32: return "cf32_le";
  }
  return "?";
}

std::size_t iq_bytes_per_sample(IqFormat f) noexcept {
  switch (f) {
    case IqFormat::kCu8:
    case IqFormat::kCs8: return 2;
    case IqFormat::kCs16: return 4;
    case IqFormat::kCf32: return 8;
  }
  return 0;
}

std::optional<IqFormat> parse_iq_format(std::string_view s) noexcept {
  if (s == "cu8" || s == "uc8") return IqFormat::kCu8;
  if (s == "cs8" || s == "ci8" || s == "sc8") return IqFormat::kCs8;
  if (s == "cs16" || s == "ci16" || s == "sc16") return IqFormat::kCs16;
  if (s == "cf32" || s == "fc32") return IqFormat::kCf32;
  return std::nullopt;
}

bool validate_iq_config(const IqConfig& c, std::string* why) {
  char buf[160];
  auto check = [&](bool ok, const char* what) {
    if (!ok && why) *why = what;
    return ok;
  };
  auto in = [&](double v, double lo, double hi, const char* name) {
    if (std::isfinite(v) && v >= lo && v <= hi) return true;
    std::snprintf(buf, sizeof buf, "%s must be in [%g, %g] (got %g)", name, lo, hi, v);
    if (why) *why = buf;
    return false;
  };
  if (!in(c.sample_rate_hz, 1e5, 1e9, "sample rate") || !in(c.center_hz, 1e6, 1e11, "center frequency") ||
      !check(c.oversample >= 1 && c.oversample <= 64, "oversample must be in [1, 64]") ||
      !in(c.antenna_height_m, 0, 1000, "antenna height") || !in(c.cable_loss_db, 0, 100, "cable loss") ||
      !in(c.noise_figure_db, 0, 50, "noise figure") || !in(c.full_scale_dbm, -200, 50, "full scale") ||
      !in(c.rx_ppm, -1000, 1000, "ppm") || !in(c.lo_drift_ppm_per_s, -1, 1, "LO drift") ||
      !in(c.dc_i, -1, 1, "DC offset I") || !in(c.dc_q, -1, 1, "DC offset Q") ||
      !in(c.iq_gain_imbalance, -0.5, 0.5, "IQ gain imbalance") || !in(c.iq_phase_deg, -45, 45, "IQ phase") ||
      !in(c.tx_power_min_dbm, -50, 90, "tx power min") || !in(c.tx_power_max_dbm, c.tx_power_min_dbm, 90, "tx power max") ||
      !in(c.tx_offset_sd_hz, 0, 1e6, "tx offset spread") || !in(c.tx_drift_sd_hz_per_s, 0, 1e4, "tx drift spread") ||
      !in(c.tx_jitter_sd_hz, 0, 1e5, "tx jitter") || !in(c.rise_us, 1e-3, 1, "rise time") ||
      !in(c.fall_us, 1e-3, 1, "fall time") || !in(c.k_factor, 0.1, 100, "k-factor") ||
      !in(c.diffraction_db_per_km, 0, 100, "diffraction") || !in(c.ground_reflection, 0, 1, "ground reflection") ||
      !in(c.min_snr_db, -100, 100, "min SNR") || !in(c.tail_s, 0, 3600, "tail") ||
      !check(c.reflectors.size() <= static_cast<std::size_t>(kMaxIqPaths - 2), "at most 6 reflectors") ||
      !in(c.modeac_rate_hz, 0, 1000, "Mode A/C rate") ||
      !check(c.modeac_radars >= 1 && c.modeac_radars <= 64, "Mode A/C radars must be in [1, 64]") ||
      !in(c.modeac_beam_deg, 0.1, 360, "Mode A/C beam width") ||
      !in(c.modeac_mode_c_share, 0, 1, "Mode C share") ||
      !in(c.modeac_reply_efficiency, 0.01, 1, "Mode A/C reply efficiency") ||
      !in(c.modeac_spi_share, 0, 1, "SPI share")) {
    return false;
  }
  for (const Reflector& r : c.reflectors) {
    if (!in(r.lat, -90, 90, "reflector latitude") || !in(r.lon, -180, 180, "reflector longitude") ||
        !in(r.alt_m, -500, 20000, "reflector altitude") || !in(r.loss_db, 0, 200, "reflector loss")) {
      return false;
    }
  }
  return true;
}

LinkResult compute_link(const Site& site, const Emitter& e, const IqConfig& cfg) {
  LinkResult r;
  const double h_r = std::max(site.antenna_height_m, 0.5);
  const double h_t = std::max(e.alt_m - site.ground_alt_m, 0.5);
  const Vec3 rx = ecef(site.lat, site.lon, site.ground_alt_m + h_r);
  const Vec3 tx = ecef(e.lat, e.lon, e.alt_m);
  const Vec3 los = tx - rx;
  r.range_m = norm(los);
  r.ground_distance_m = ground_distance(site.lat, site.lon, e.lat, e.lon);
  const Vec3 v = enu_to_ecef(e.lat, e.lon, e.vel_east_mps, e.vel_north_mps, e.vel_up_mps);
  r.range_rate_mps = r.range_m > 0 ? dot(v, los) / r.range_m : 0.0;

  // Effective-Earth geometry: rays bend with the k-factor, which is the same
  // as straight rays over an Earth of radius k R.
  const double re = cfg.k_factor * kMeanEarthRadius;
  const double d = r.ground_distance_m;
  const double el = pm::atan2(h_t - h_r - d * d / (2.0 * re), std::max(d, 1e-3));
  r.elevation_deg = el / kDeg;
  r.horizon_m = std::sqrt(2.0 * re * h_t) + std::sqrt(2.0 * re * h_r);
  const double beyond_km = std::max(0.0, d - r.horizon_m) / 1000.0;

  const double f_tx = cfg.center_hz + e.carrier_offset_hz;
  r.antenna_gain_db = antenna_gain_db(cfg.antenna, el);
  r.path_loss_db = fspl_db(r.range_m, f_tx) + cfg.diffraction_db_per_km * beyond_km;
  r.power_dbm = e.power_dbm + r.antenna_gain_db - r.path_loss_db - cfg.cable_loss_db;

  const double f_lo = cfg.center_hz * (1.0 + cfg.rx_ppm * 1e-6);
  // A copy delayed by tau, mixed down by the LO, is x(t - tau) times
  // exp(-j 2 pi f_lo tau): the envelope (and its baseband rotation) is
  // delayed, and the carrier adds a phase from the LO frequency.
  auto delay_phase = [&](double extra_m) { return pm::kPi - pm::kTwoPi * frac(extra_m * f_lo / kC); };
  auto baseband = [&](double rdot) {
    const double rd = cfg.doppler ? rdot : 0.0;
    return f_tx * (1.0 - rd / kC) - f_lo;
  };
  r.doppler_hz = cfg.doppler ? -f_tx * r.range_rate_mps / kC : 0.0;
  r.baseband_hz = baseband(r.range_rate_mps);

  const double amp = std::sqrt(pm::db_to_power(r.power_dbm - cfg.full_scale_dbm));
  r.paths[r.n_paths++] = {0.0, amp, 0.0, r.baseband_hz};

  // Two-ray ground bounce over the same effective (k R) Earth as the horizon:
  // specular point, path difference and divergence factor. None beyond the
  // horizon (the ground hides the reflection point).
  Bounce b;
  if (cfg.ground_reflection > 0 && d <= r.horizon_m && spherical_bounce(h_r, h_t, d, re, b)) {
    const double g = std::sqrt(pm::db_to_power(antenna_gain_db(cfg.antenna, -b.grazing_rad) - r.antenna_gain_db));
    r.paths[r.n_paths++] = {b.delta_m / kC, amp * cfg.ground_reflection * b.divergence * g,
                            delay_phase(b.delta_m), r.baseband_hz};
  }

  for (const Reflector& refl : cfg.reflectors) {
    if (r.n_paths >= kMaxIqPaths) break;
    const Vec3 p = ecef(refl.lat, refl.lon, refl.alt_m);
    const Vec3 leg1 = tx - p;
    const Vec3 leg2 = p - rx;
    const double l1 = norm(leg1), l2 = norm(leg2);
    const double total = l1 + l2;
    // Elevation of the reflector seen from the receiver.
    const double dr = ground_distance(site.lat, site.lon, refl.lat, refl.lon);
    const double h_p = std::max(refl.alt_m - site.ground_alt_m, 0.5);
    const double el_r = pm::atan2(h_p - h_r - dr * dr / (2.0 * re), std::max(dr, 1e-3));
    // Each leg loses the same diffraction per km beyond its own horizon.
    const double dt = ground_distance(e.lat, e.lon, refl.lat, refl.lon);
    const double beyond1 = std::max(0.0, dt - std::sqrt(2.0 * re * h_t) - std::sqrt(2.0 * re * h_p));
    const double beyond2 = std::max(0.0, dr - std::sqrt(2.0 * re * h_r) - std::sqrt(2.0 * re * h_p));
    const double p_dbm = e.power_dbm + antenna_gain_db(cfg.antenna, el_r) - fspl_db(total, f_tx) - refl.loss_db -
                         cfg.diffraction_db_per_km * (beyond1 + beyond2) / 1000.0 - cfg.cable_loss_db;
    const double rdot_leg = l1 > 0 ? dot(v, leg1) / l1 : 0.0;  // the reflector does not move
    r.paths[r.n_paths++] = {(total - r.range_m) / kC, std::sqrt(pm::db_to_power(p_dbm - cfg.full_scale_dbm)),
                            delay_phase(total - r.range_m), baseband(rdot_leg)};
  }
  return r;
}

IqPlan plan_iq(const SynthConfig& synth, const SynthOutput& traffic, const IqConfig& cfg) {
  IqPlan plan;
  const double fs_act = cfg.sample_rate_hz * (1.0 + cfg.rx_ppm * 1e-6);
  plan.noise_dbm = -174.0 + pm::power_to_db(cfg.sample_rate_hz) + cfg.noise_figure_db;
  plan.noise_sigma = std::sqrt(pm::db_to_power(plan.noise_dbm - cfg.full_scale_dbm) / 2.0);
  // At least the traffic's duration plus the tail; extended below so every
  // message ends inside the recording.
  plan.total_samples =
      static_cast<std::uint64_t>(std::ceil((std::max(0.0, synth.duration_s) + std::max(0.0, cfg.tail_s)) * fs_act));

  Site site{synth.receiver_lat, synth.receiver_lon, static_cast<double>(synth.receiver_alt_m),
            cfg.antenna_height_m};

  std::unordered_map<std::uint32_t, std::size_t> by_icao;
  for (const AircraftInfo& a : traffic.aircraft) {
    Rng r(mix(cfg.seed, 0x7700000000ULL + a.icao));
    IqTransmitter t;
    t.icao = a.icao;
    t.category = a.category;
    t.power_dbm = r.uniform(cfg.tx_power_min_dbm, std::max(cfg.tx_power_min_dbm, cfg.tx_power_max_dbm));
    t.offset_hz = std::clamp(gaussian(r) * cfg.tx_offset_sd_hz, -1e6, 1e6);
    t.drift_hz_per_s = gaussian(r) * cfg.tx_drift_sd_hz_per_s;
    by_icao.emplace(a.icao, plan.transmitters.size());
    plan.transmitters.push_back(t);
  }

  const double edge = std::max(cfg.rise_us, cfg.fall_us) * 1e-6;
  // Plans one message (Mode S, or Mode A/C when mode_ac is 'A' or 'C').
  auto place = [&](const Transmission& tr, char mode_ac) {
    const auto it = by_icao.find(tr.icao);
    if (it == by_icao.end()) return;
    const IqTransmitter& xmit = plan.transmitters[it->second];
    // Per-message draws depend on the message itself, not its index, so a
    // message renders the same whatever other traffic there is. Mode A/C
    // replies draw from their own stream.
    Rng mr(mix(cfg.seed, mix((mode_ac ? 0xAC51000000000000ULL : 0x5100000000000000ULL) + tr.icao, tr.tx_ns)));

    const double t = static_cast<double>(tr.tx_ns - synth.start_rx_ns) * 1e-9;
    Emitter e;
    e.lat = tr.lat;
    e.lon = tr.lon;
    e.alt_m = tr.on_ground ? synth.receiver_alt_m + kGroundAntennaM : tr.alt_ft * kFtToM;
    double st, ct;
    pm::sincos(tr.track_deg * kDeg, st, ct);
    e.vel_east_mps = tr.ground_speed_kt * kKtToMps * st;
    e.vel_north_mps = tr.ground_speed_kt * kKtToMps * ct;
    e.vel_up_mps = tr.on_ground ? 0.0 : tr.vertical_rate_fpm * kFpmToMps;
    e.power_dbm = xmit.power_dbm;
    e.carrier_offset_hz = xmit.offset_hz + xmit.drift_hz_per_s * t + gaussian(mr) * cfg.tx_jitter_sd_hz;
    const double phase0 = pm::kTwoPi * mr.uniform();

    IqMessage m;
    m.link = compute_link(site, e, cfg);
    if (cfg.lo_drift_ppm_per_s != 0.0) {
      // The LO is higher by center * drift * t: every path's baseband is lower.
      const double shift = cfg.center_hz * cfg.lo_drift_ppm_per_s * 1e-6 * t;
      m.link.baseband_hz -= shift;
      for (int p = 0; p < m.link.n_paths; ++p) m.link.paths[p].freq_hz -= shift;
    }
    if (m.link.ground_distance_m > m.link.horizon_m) ++plan.stats.beyond_horizon;
    m.snr_db = m.link.power_dbm - plan.noise_dbm;
    // Skipped only if every path is weak (an echo can outlive the direct path).
    double strongest = 0.0;
    for (int p = 0; p < m.link.n_paths; ++p) strongest = std::max(strongest, m.link.paths[p].amplitude);
    const double strongest_snr_db =
        strongest > 0 ? pm::power_to_db(strongest * strongest) + cfg.full_scale_dbm - plan.noise_dbm : -999.0;
    if (strongest_snr_db < cfg.min_snr_db) {
      ++plan.stats.too_weak;
      if (mode_ac) ++plan.stats.modeac_too_weak;
      return;
    }
    m.tx_ns = tr.tx_ns;
    m.icao = tr.icao;
    m.len = tr.len;
    m.mode_ac = mode_ac;
    std::memcpy(m.bytes, tr.bytes, tr.len);
    m.lat = tr.lat;
    m.lon = tr.lon;
    m.alt_ft = tr.alt_ft;
    m.tx_offset_hz = e.carrier_offset_hz;
    m.observed_hz = m.link.baseband_hz / (1.0 + cfg.rx_ppm * 1e-6);
    m.phase0 = phase0;
    m.arrival_s = t + m.link.range_m / kC;
    m.sample_pos = m.arrival_s * fs_act;
    double max_delay = 0.0;
    for (int p = 0; p < m.link.n_paths; ++p) max_delay = std::max(max_delay, m.link.paths[p].delay_s);
    m.first_sample = static_cast<std::int64_t>(std::floor((m.arrival_s - edge) * fs_act)) - 1;
    m.last_sample = static_cast<std::int64_t>(
                        std::ceil((m.arrival_s + max_delay + message_duration_s(m) + edge) * fs_act)) + 1;
    m.first_sample = std::max<std::int64_t>(m.first_sample, 0);
    plan.total_samples = std::max(plan.total_samples, static_cast<std::uint64_t>(m.last_sample) + 1);
    if (mode_ac) ++plan.stats.modeac_replies;
    plan.messages.push_back(m);
  };
  for (std::size_t i = 0; i < traffic.transmissions.size(); ++i) {
    ++plan.stats.transmissions;
    place(traffic.transmissions[i], 0);
  }
  if (cfg.modeac_rate_hz > 0.0) {
    for (const ModeAcReply& r : plan_mode_ac(synth, traffic, cfg)) place(r.tr, r.mode);
  }

  std::stable_sort(plan.messages.begin(), plan.messages.end(),
                   [](const IqMessage& a, const IqMessage& b) { return a.first_sample < b.first_sample; });

  // Collisions: any overlap of two rendered spans.
  for (std::size_t i = 0; i < plan.messages.size(); ++i) {
    IqMessage& a = plan.messages[i];
    for (std::size_t j = i + 1; j < plan.messages.size() && plan.messages[j].first_sample <= a.last_sample; ++j) {
      IqMessage& b = plan.messages[j];
      a.collided = b.collided = true;
      a.interferer_db = std::max(a.interferer_db, b.snr_db - a.snr_db);
      b.interferer_db = std::max(b.interferer_db, a.snr_db - b.snr_db);
    }
  }
  for (const IqMessage& m : plan.messages) {
    if (m.collided) ++plan.stats.collided;
    plan.max_span_samples = std::max(plan.max_span_samples, m.last_sample - m.first_sample + 1);
  }
  plan.stats.rendered = plan.messages.size();
  return plan;
}

std::size_t render_iq_chunk(const IqPlan& plan, const IqConfig& cfg, std::uint64_t chunk, float* out) {
  const std::uint64_t n0 = chunk * kIqChunkSamples;
  if (n0 >= plan.total_samples) return 0;
  const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kIqChunkSamples, plan.total_samples - n0));

  thread_local std::vector<double> acc;
  acc.assign(2 * n, 0.0);

  const double fs_act = cfg.sample_rate_hz * (1.0 + cfg.rx_ppm * 1e-6);
  const double dt = 1.0 / fs_act;
  const int k_over = std::clamp(cfg.oversample, 1, 64);
  const double dsub = dt / k_over;
  const double rise = std::max(cfg.rise_us, 1e-3) * 1e-6;
  const double fall = std::max(cfg.fall_us, 1e-3) * 1e-6;

  // Messages whose span can reach this chunk: they start at most
  // max_span_samples before it.
  const auto lo = static_cast<std::int64_t>(n0) - plan.max_span_samples;
  auto it = std::lower_bound(plan.messages.begin(), plan.messages.end(), lo,
                             [](const IqMessage& m, std::int64_t v) { return m.first_sample < v; });
  const auto end_sample = static_cast<std::int64_t>(n0 + n) - 1;
  Interval pulses[4 + 112];
  for (; it != plan.messages.end() && it->first_sample <= end_sample; ++it) {
    const IqMessage& m = *it;
    if (m.last_sample < static_cast<std::int64_t>(n0)) continue;
    const int np = m.mode_ac ? mode_ac_intervals(m.bytes, pulses) : pulse_intervals(m.bytes, m.len, pulses);
    const std::int64_t s_begin = std::max<std::int64_t>(m.first_sample, static_cast<std::int64_t>(n0));
    const std::int64_t s_end = std::min<std::int64_t>(m.last_sample, end_sample);
    for (int p = 0; p < m.link.n_paths; ++p) {
      const IqPath& path = m.link.paths[p];
      const double ta = m.arrival_s + path.delay_s;
      // Time of the first point, relative to this path's preamble start.
      double trel = static_cast<double>(s_begin) * dt - dt / 2 + dsub / 2 - ta;
      double pr, pi;  // carrier phasor
      pm::sincos(m.phase0 + path.phase_rad + pm::kTwoPi * path.freq_hz * trel, pi, pr);
      double rr, ri;  // rotation per point
      pm::sincos(pm::kTwoPi * path.freq_hz * dsub, ri, rr);
      const double scale = path.amplitude / k_over;
      int q = 0;  // first pulse that has not ended yet
      for (std::int64_t s = s_begin; s <= s_end; ++s) {
        double si = 0.0, sq = 0.0;
        for (int k = 0; k < k_over; ++k) {
          while (q < np && trel > pulses[q].b + fall / 2) ++q;
          if (q < np) {
            const double up = (trel - pulses[q].a) / rise + 0.5;
            const double down = (pulses[q].b - trel) / fall + 0.5;
            const double env = std::clamp(std::min(up, down), 0.0, 1.0);
            si += env * pr;
            sq += env * pi;
          }
          const double nr = pr * rr - pi * ri;
          pi = pr * ri + pi * rr;
          pr = nr;
          trel += dsub;
        }
        const auto idx = static_cast<std::size_t>(s - static_cast<std::int64_t>(n0));
        acc[2 * idx] += si * scale;
        acc[2 * idx + 1] += sq * scale;
      }
    }
  }

  if (cfg.noise) {
    Rng r(mix(cfg.seed, 0x40000000000000ULL + chunk));
    for (std::size_t i = 0; i < n; ++i) {
      // One Box-Muller draw gives both components.
      const double u1 = 1.0 - r.uniform();
      const double u2 = r.uniform();
      const double rad = std::sqrt(-2.0 * pm::log(u1)) * plan.noise_sigma;
      double s, c;
      pm::sincos(pm::kTwoPi * u2, s, c);
      acc[2 * i] += rad * c;
      acc[2 * i + 1] += rad * s;
    }
  }

  // Analog impairments after the noise (the mixer sees the front end's noise).
  double sphi = 0.0, cphi = 1.0;
  pm::sincos(cfg.iq_phase_deg * kDeg, sphi, cphi);
  const double qgain = 1.0 + cfg.iq_gain_imbalance;
  for (std::size_t i = 0; i < n; ++i) {
    const double ii = acc[2 * i];
    const double qq = acc[2 * i + 1];
    out[2 * i] = static_cast<float>(ii + cfg.dc_i);
    out[2 * i + 1] = static_cast<float>(qgain * (qq * cphi + ii * sphi) + cfg.dc_q);
  }
  return n;
}

std::uint64_t quantize_iq(const float* iq, std::size_t n, IqFormat format, std::uint8_t* dst) {
  std::uint64_t clipped = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x[2] = {static_cast<double>(iq[2 * i]), static_cast<double>(iq[2 * i + 1])};
    bool clip = false;
    for (std::size_t c = 0; c < 2; ++c) {
      double v = x[c];
      if (!(v == v)) v = 0.0;  // NaN cannot come from a validated config; never cast it
      if (v > 1.0 || v < -1.0 || x[c] != x[c]) clip = true;
      switch (format) {
        case IqFormat::kCu8: {
          const double y = std::clamp(std::floor(v * 127.5 + 128.0), 0.0, 255.0);
          dst[2 * i + c] = static_cast<std::uint8_t>(y);
          break;
        }
        case IqFormat::kCs8: {
          const double y = std::clamp(std::floor(v * 127.0 + 0.5), -128.0, 127.0);
          dst[2 * i + c] = static_cast<std::uint8_t>(static_cast<std::int8_t>(y));
          break;
        }
        case IqFormat::kCs16: {
          const double y = std::clamp(std::floor(v * 32767.0 + 0.5), -32768.0, 32767.0);
          const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(y));
          dst[4 * i + 2 * c] = static_cast<std::uint8_t>(u & 0xFF);
          dst[4 * i + 2 * c + 1] = static_cast<std::uint8_t>(u >> 8);
          break;
        }
        case IqFormat::kCf32: {
          const auto u = std::bit_cast<std::uint32_t>(static_cast<float>(v));
          for (std::size_t b = 0; b < 4; ++b) dst[8 * i + 4 * c + b] = static_cast<std::uint8_t>(u >> (8 * b));
          break;
        }
      }
    }
    if (clip) ++clipped;
  }
  return clipped;
}

std::string sigmf_meta(const SynthConfig& synth, const IqConfig& cfg, const IqPlan& plan) {
  Fmt j;
  j.s.reserve(512 + plan.messages.size() * 700);
  j.add("{\n  \"global\": {\n");
  j.add("    \"core:datatype\": \"%s\",\n", sigmf_datatype(cfg.format));
  j.add("    \"core:sample_rate\": %.3f,\n", cfg.sample_rate_hz);
  j.add("    \"core:version\": \"1.0.0\",\n");
  j.add("    \"core:num_channels\": 1,\n");
  j.add("    \"core:recorder\": \"adsb-iqsynth\",\n");
  j.add("    \"core:hw\": \"synthetic 1090 MHz receiver\",\n");
  j.add("    \"core:description\": \"Synthetic Mode S / ADS-B scenario: %d aircraft, %.3f s. "
        "Generated; never transmit it over the air.\",\n",
        synth.aircraft, synth.duration_s);
  j.add("    \"core:extensions\": [{\"name\": \"adsb\", \"version\": \"0.1.0\", \"optional\": true}],\n");
  j.add("    \"adsb:receiver\": {\"lat\": %.7f, \"lon\": %.7f, \"ground_alt_m\": %d, \"antenna_height_m\": %.2f, "
        "\"antenna\": \"%s\", \"cable_loss_db\": %.2f, \"noise_figure_db\": %.2f, \"full_scale_dbm\": %.2f, "
        "\"ppm\": %.4f, \"lo_drift_ppm_per_s\": %.6f, \"dc_i\": %.5f, \"dc_q\": %.5f, \"iq_gain_imbalance\": %.5f, "
        "\"iq_phase_deg\": %.4f},\n",
        synth.receiver_lat, synth.receiver_lon, synth.receiver_alt_m, cfg.antenna_height_m,
        cfg.antenna == AntennaPattern::kDipole ? "vertical dipole" : "omni", cfg.cable_loss_db,
        cfg.noise_figure_db, cfg.full_scale_dbm, cfg.rx_ppm, cfg.lo_drift_ppm_per_s, cfg.dc_i, cfg.dc_q,
        cfg.iq_gain_imbalance,
        cfg.iq_phase_deg);
  j.add("    \"adsb:noise\": {\"enabled\": %s, \"dbm\": %.3f, \"sigma_fs\": %.8f},\n", cfg.noise ? "true" : "false",
        plan.noise_dbm, plan.noise_sigma);
  j.add("    \"adsb:channel\": {\"doppler\": %s, \"k_factor\": %.6f, \"diffraction_db_per_km\": %.3f, "
        "\"ground_reflection\": %.3f, \"reflectors\": [",
        cfg.doppler ? "true" : "false", cfg.k_factor, cfg.diffraction_db_per_km, cfg.ground_reflection);
  for (std::size_t i = 0; i < cfg.reflectors.size(); ++i) {
    const Reflector& r = cfg.reflectors[i];
    j.add("%s{\"lat\": %.7f, \"lon\": %.7f, \"alt_m\": %.2f, \"loss_db\": %.2f}", i ? ", " : "", r.lat, r.lon,
          r.alt_m, r.loss_db);
  }
  j.add("]},\n");
  j.add("    \"adsb:generator\": {\"seed\": %llu, \"traffic_seed\": %llu, \"oversample\": %d, \"rise_us\": %.3f, "
        "\"fall_us\": %.3f, \"tx_offset_sd_hz\": %.3f, \"tx_drift_sd_hz_per_s\": %.4f, \"tx_jitter_sd_hz\": %.3f, "
        "\"min_snr_db\": %.2f, \"tail_s\": %.3f, \"start_rx_ns\": %llu},\n",
        static_cast<unsigned long long>(cfg.seed), static_cast<unsigned long long>(synth.seed), cfg.oversample,
        cfg.rise_us, cfg.fall_us, cfg.tx_offset_sd_hz, cfg.tx_drift_sd_hz_per_s, cfg.tx_jitter_sd_hz,
        cfg.min_snr_db, cfg.tail_s, static_cast<unsigned long long>(synth.start_rx_ns));
  j.add("    \"adsb:stats\": {\"transmissions\": %llu, \"rendered\": %llu, \"too_weak\": %llu, "
        "\"beyond_horizon\": %llu, \"collided\": %llu, \"samples\": %llu},\n",
        static_cast<unsigned long long>(plan.stats.transmissions),
        static_cast<unsigned long long>(plan.stats.rendered), static_cast<unsigned long long>(plan.stats.too_weak),
        static_cast<unsigned long long>(plan.stats.beyond_horizon),
        static_cast<unsigned long long>(plan.stats.collided),
        static_cast<unsigned long long>(plan.total_samples));
  if (cfg.modeac_rate_hz > 0.0) {
    j.add("    \"adsb:mode_ac\": {\"rate_hz\": %.4f, \"radars\": %d, \"beam_deg\": %.3f, \"mode_c_share\": %.4f, "
          "\"reply_efficiency\": %.4f, \"spi_share\": %.4f, \"replies\": %llu, \"too_weak\": %llu},\n",
          cfg.modeac_rate_hz, cfg.modeac_radars, cfg.modeac_beam_deg, cfg.modeac_mode_c_share,
          cfg.modeac_reply_efficiency, cfg.modeac_spi_share, static_cast<unsigned long long>(plan.stats.modeac_replies),
          static_cast<unsigned long long>(plan.stats.modeac_too_weak));
  }
  j.add("    \"adsb:transmitters\": [");
  for (std::size_t i = 0; i < plan.transmitters.size(); ++i) {
    const IqTransmitter& t = plan.transmitters[i];
    j.add("%s\n      {\"icao\": \"%06X\", \"category\": \"%02X\", \"power_dbm\": %.2f, \"offset_hz\": %.3f, "
          "\"drift_hz_per_s\": %.5f}",
          i ? "," : "", t.icao, t.category, t.power_dbm, t.offset_hz, t.drift_hz_per_s);
  }
  j.add("\n    ]\n  },\n");
  j.add("  \"captures\": [{\"core:sample_start\": 0, \"core:frequency\": %.3f}],\n", cfg.center_hz);
  j.add("  \"annotations\": [");
  for (std::size_t i = 0; i < plan.messages.size(); ++i) {
    const IqMessage& m = plan.messages[i];
    char hex[29];
    for (int b = 0; b < m.len; ++b) std::snprintf(hex + 2 * b, 3, "%02X", m.bytes[b]);
    const int df = m.bytes[0] >> 3;
    const double f = cfg.center_hz + m.observed_hz;
    char label[8];
    if (m.mode_ac) std::snprintf(label, sizeof label, "MODE%c", m.mode_ac);
    else std::snprintf(label, sizeof label, "DF%d", df);
    j.add("%s\n    {\"core:sample_start\": %lld, \"core:sample_count\": %lld, \"core:freq_lower_edge\": %.1f, "
          "\"core:freq_upper_edge\": %.1f, \"core:label\": \"%s\", \"core:generator\": \"adsb-iqsynth\", ",
          i ? "," : "", static_cast<long long>(m.first_sample),
          static_cast<long long>(m.last_sample - m.first_sample + 1), f - 0.5e6, f + 0.5e6, label);
    if (m.mode_ac) {
      // Code word as readsb's Beast type '1' payload (adsb:hex), and what it says.
      const auto code = static_cast<std::uint16_t>(m.bytes[0] << 8 | m.bytes[1]);
      j.add("\"adsb:mode_ac\": \"%c\", \"adsb:spi\": %s, ", m.mode_ac,
            (code & modes::kModeAcSpiBit) ? "true" : "false");
      if (m.mode_ac == 'A') {
        j.add("\"adsb:squawk\": \"%04X\", ", code & 0x7777u);
      } else if (const auto ft = modes::gillham_to_feet(code)) {
        j.add("\"adsb:modec_alt_ft\": %d, ", static_cast<int>(*ft));
      } else {
        j.add("\"adsb:modec_alt_ft\": null, ");
      }
    }
    j.add("\"adsb:hex\": \"%s\", \"adsb:icao\": \"%06X\", \"adsb:tx_ns\": %llu, \"adsb:sample_pos\": %.4f, ", hex,
          m.icao, static_cast<unsigned long long>(m.tx_ns), m.sample_pos);
    j.add("\"adsb:snr_db\": %.2f, \"adsb:power_dbm\": %.2f, \"adsb:range_m\": %.1f, \"adsb:elevation_deg\": %.3f, "
          "\"adsb:range_rate_mps\": %.3f, ",
          m.snr_db, m.link.power_dbm, m.link.range_m, m.link.elevation_deg, m.link.range_rate_mps);
    j.add("\"adsb:tx_offset_hz\": %.3f, \"adsb:doppler_hz\": %.3f, \"adsb:observed_hz\": %.3f, "
          "\"adsb:paths\": %d, \"adsb:collided\": %s, ",
          m.tx_offset_hz, m.link.doppler_hz, m.observed_hz, m.link.n_paths, m.collided ? "true" : "false");
    if (m.collided) j.add("\"adsb:interferer_db\": %.2f, ", m.interferer_db);
    else j.add("\"adsb:interferer_db\": null, ");
    j.add("\"adsb:lat\": %.7f, \"adsb:lon\": %.7f, \"adsb:alt_ft\": %.1f}", m.lat, m.lon, m.alt_ft);
  }
  j.add("\n  ]\n}\n");
  return j.s;
}

}  // namespace adsb::sim
