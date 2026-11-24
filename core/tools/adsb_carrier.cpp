// adsb-carrier: carrier frequency of every decoded message in an IQ
// recording, with the Doppler from the aircraft's own decoded track
// (transponder frequency-stability study, plan §22.5).
//
//   adsb-carrier INPUT [options] > messages.csv
//
// INPUT is a SigMF recording (NAME.sigmf-data / NAME.sigmf-meta) or a raw IQ
// file / "-" with --format and --rate. For each frame the demodulator finds,
// the carrier is estimated from the IQ samples (adsb/demod/carrier.hpp); the
// frame also goes through the track engine, and when the aircraft has a
// position (at most --max-age old, moved forward with its velocity) and a
// velocity, the Doppler is computed from the range rate to the receiver.
// One CSV row per frame (see the header line); scripts/carrier-study.py
// removes the receiver's own error and groups the results by aircraft.
//
// The receiver position comes from --receiver, or from the SigMF metadata's
// adsb:receiver (the generator's).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "adsb/common/build_config.hpp"
#include "adsb/common/portable_math.hpp"
#include "adsb/demod/carrier.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/ingest/iq_source.hpp"
#include "adsb/protocol/feed_messages.hpp"

namespace {

using adsb::demod::SampleFormat;

void usage() {
  std::fprintf(stderr,
               "usage: adsb-carrier INPUT [options] (CSV on stdout)\n"
               "  INPUT                 NAME.sigmf-data / NAME.sigmf-meta, a raw IQ file, or - (stdin)\n"
               "  --format F            cu8 | cs8 | cs16 | cf32 (raw input; SigMF gives it)\n"
               "  --rate HZ             sample rate (raw input; default 2.4e6)\n"
               "  --center HZ           tuned frequency (default: SigMF core:frequency, else 1090e6)\n"
               "  --receiver LAT,LON[,ALT_M]  receiver antenna (default: the SigMF adsb:receiver)\n"
               "  --out FILE|-          CSV output (default -)\n"
               "  --max-age S           newest position may be this old (default 10 s)\n"
               "  --min-cover F         use samples at least this much covered by pulses (default 0.5)\n"
               "  --clip-level F        leave out samples with |I| or |Q| >= F of full scale (default 0.99)\n"
               "  --no-dc               do not subtract each block's mean (DC offset)\n"
               "  --fix                 correct one wrong bit in DF11/17/18 frames\n"
               "  --quiet               no statistics on stderr\n");
}

bool parse_number(const char* v, double& out) {
  char* end = nullptr;
  out = std::strtod(v, &end);
  return end != v && *end == '\0' && std::isfinite(out);
}

// "key": value after `from` in a JSON text (numbers only).
bool json_number_after(const std::string& text, std::size_t from, const char* key, double& out) {
  const std::size_t k = text.find(std::string("\"") + key + "\"", from);
  if (k == std::string::npos) return false;
  std::size_t i = text.find(':', k);
  if (i == std::string::npos) return false;
  char* end = nullptr;
  out = std::strtod(text.c_str() + i + 1, &end);
  return end != text.c_str() + i + 1;
}

// WGS-84 geodetic -> ECEF (m).
void ecef(double lat_deg, double lon_deg, double h_m, double out[3]) {
  constexpr double a = 6378137.0;
  constexpr double f = 1.0 / 298.257223563;
  constexpr double e2 = f * (2.0 - f);
  const double lat = lat_deg * adsb::pm::kPi / 180.0, lon = lon_deg * adsb::pm::kPi / 180.0;
  const double n = a / std::sqrt(1.0 - e2 * std::sin(lat) * std::sin(lat));
  out[0] = (n + h_m) * std::cos(lat) * std::cos(lon);
  out[1] = (n + h_m) * std::cos(lat) * std::sin(lon);
  out[2] = (n * (1.0 - e2) + h_m) * std::sin(lat);
}

// East/north/up velocity at (lat, lon) -> ECEF.
void enu_to_ecef(double lat_deg, double lon_deg, double e, double n, double u, double out[3]) {
  const double lat = lat_deg * adsb::pm::kPi / 180.0, lon = lon_deg * adsb::pm::kPi / 180.0;
  const double sl = std::sin(lat), cl = std::cos(lat), so = std::sin(lon), co = std::cos(lon);
  out[0] = -so * e - sl * co * n + cl * co * u;
  out[1] = co * e - sl * so * n + cl * so * u;
  out[2] = cl * n + sl * u;
}

struct Track {
  adsb::protocol::TrackUpdate u;
  std::uint64_t pos_ns = 0;  // event time of the latest accepted position fix
  bool has_pos = false;
};

struct Sink {
  std::unordered_map<std::uint32_t, Track>* tracks;
  void on_update(const adsb::protocol::TrackUpdate& u) {
    Track& t = (*tracks)[u.icao];
    t.u = u;
    // The position's time is set from the engine's fix counters in handle():
    // an update carries kPosition only when the position changed, and an
    // aircraft that is not moving (or moves less than the CPR resolution)
    // keeps sending fixes that change nothing.
    if (u.update_mask & adsb::protocol::update_mask::kPosition) t.has_pos = true;
  }
  void on_delete(const adsb::protocol::TrackDelete& d) { tracks->erase(d.icao); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string input = argv[1];
  if (input == "-h" || input == "--help") {
    usage();
    return 0;
  }
  adsb::demod::DemodConfig dcfg;
  adsb::demod::CarrierConfig ccfg;
  std::string format_arg, out_path = "-";
  bool rate_given = false, quiet = false, dc = true, have_rx = false, center_given = false;
  double center_hz = 1090e6, max_age_s = 10.0;
  double rx[3] = {0, 0, 0};
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "adsb-carrier: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    auto num = [&]() {
      const char* v = next();
      double d;
      if (!parse_number(v, d)) {
        std::fprintf(stderr, "adsb-carrier: bad number for %s: %s\n", a.c_str(), v);
        std::exit(2);
      }
      return d;
    };
    if (a == "--format") format_arg = next();
    else if (a == "--rate") { dcfg.sample_rate_hz = num(); rate_given = true; }
    else if (a == "--center") { center_hz = num(); center_given = true; }
    else if (a == "--receiver") {
      const std::string v = next();
      double vals[3] = {0, 0, 0};
      int n = 0;
      std::size_t p = 0;
      while (n < 3) {
        const std::size_t c = v.find(',', p);
        const std::string part = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
        if (!parse_number(part.c_str(), vals[n])) {
          n = -1;
          break;
        }
        ++n;
        if (c == std::string::npos) break;
        p = c + 1;
      }
      if (n < 2 || std::fabs(vals[0]) > 90 || std::fabs(vals[1]) > 180) {
        std::fprintf(stderr, "adsb-carrier: --receiver LAT,LON[,ALT_M]\n");
        return 2;
      }
      rx[0] = vals[0];
      rx[1] = vals[1];
      rx[2] = vals[2];
      have_rx = true;
    }
    else if (a == "--out") out_path = next();
    else if (a == "--max-age") max_age_s = num();
    else if (a == "--min-cover") ccfg.min_cover = num();
    else if (a == "--clip-level") ccfg.clip_level = num();
    else if (a == "--no-dc") dc = false;
    else if (a == "--fix") dcfg.fix_single_bit = true;
    else if (a == "--quiet") quiet = true;
    else { std::fprintf(stderr, "adsb-carrier: unknown option %s\n", a.c_str()); usage(); return 2; }
  }
  if (!(max_age_s > 0 && max_age_s <= 600)) {
    std::fprintf(stderr, "adsb-carrier: --max-age must be in (0, 600] s\n");
    return 2;
  }
  if (!(center_hz > 1e6 && center_hz < 1e10)) {
    std::fprintf(stderr, "adsb-carrier: --center must be a radio frequency in Hz\n");
    return 2;
  }

  SampleFormat fmt = SampleFormat::kCu8;
  std::string data_path = input;
  const bool sigmf = input.size() > 11 && (input.compare(input.size() - 11, 11, ".sigmf-meta") == 0 ||
                                           input.compare(input.size() - 11, 11, ".sigmf-data") == 0);
  if (sigmf) {
    double rate = 0.0;
    std::string err;
    if (!adsb::ingest::read_sigmf(input, data_path, fmt, rate, &err)) {
      std::fprintf(stderr, "adsb-carrier: %s\n", err.c_str());
      return 1;
    }
    if (!rate_given) dcfg.sample_rate_hz = rate;
    std::ifstream mf(input.substr(0, input.size() - 11) + ".sigmf-meta");
    std::stringstream ss;
    ss << mf.rdbuf();
    const std::string meta = ss.str();
    if (!center_given) {
      const std::size_t c = meta.find("\"captures\"");
      double fr;
      if (c != std::string::npos && json_number_after(meta, c, "core:frequency", fr) && fr > 1e6 && fr < 1e10)
        center_hz = fr;
    }
    if (!have_rx) {
      const std::size_t r = meta.find("\"adsb:receiver\"");
      double lat, lon, ground = 0.0, ant = 0.0;
      if (r != std::string::npos && json_number_after(meta, r, "lat", lat) && json_number_after(meta, r, "lon", lon)) {
        (void)json_number_after(meta, r, "ground_alt_m", ground);
        (void)json_number_after(meta, r, "antenna_height_m", ant);
        rx[0] = lat;
        rx[1] = lon;
        rx[2] = ground + ant;
        have_rx = true;
      }
    }
  }
  if (!format_arg.empty() && !adsb::demod::parse_format(format_arg, fmt)) {
    std::fprintf(stderr, "adsb-carrier: unknown format %s\n", format_arg.c_str());
    return 2;
  }
  if (!sigmf && format_arg.empty()) {
    std::fprintf(stderr, "adsb-carrier: raw input needs --format\n");
    return 2;
  }
  ccfg.sample_rate_hz = dcfg.sample_rate_hz;
  std::string why;
  if (!adsb::demod::validate(dcfg, &why) || !adsb::demod::validate(ccfg, &why)) {
    std::fprintf(stderr, "adsb-carrier: %s\n", why.c_str());
    return 2;
  }
  if (!have_rx) std::fprintf(stderr, "adsb-carrier: no receiver position (--receiver): no Doppler\n");

  FILE* in = data_path == "-" ? stdin : std::fopen(data_path.c_str(), "rb");
  if (!in) {
    std::fprintf(stderr, "adsb-carrier: cannot open %s\n", data_path.c_str());
    return 1;
  }
  FILE* out = out_path == "-" ? stdout : std::fopen(out_path.c_str(), "w");
  if (!out) {
    std::fprintf(stderr, "adsb-carrier: cannot write %s\n", out_path.c_str());
    return 1;
  }

  adsb::demod::Demodulator demod(dcfg);
  adsb::demod::CarrierEstimator est(ccfg);
  adsb::state::EngineConfig ecfg;
  ecfg.has_receiver = have_rx;
  ecfg.receiver_lat = rx[0];
  ecfg.receiver_lon = rx[1];
  ecfg.fix_single_bit = dcfg.fix_single_bit;
  adsb::DefaultTrackEngine engine(ecfg);
  std::unordered_map<std::uint32_t, Track> tracks;
  Sink sink{&tracks};
  double rx_ecef[3];
  ecef(rx[0], rx[1], rx[2], rx_ecef);

  const double fs = dcfg.sample_rate_hz;
  const std::size_t block = dcfg.block_samples;
  const std::size_t bps = adsb::demod::bytes_per_sample(fmt);
  // History of complex samples: the current block and the two before it
  // (frames are reported up to about one message plus the overlap after
  // their start, so they lie within the last two blocks).
  const std::size_t hist_cap = 3 * block;
  std::vector<float> hist(2 * hist_cap);
  std::int64_t hist_base = 0;  // absolute index of hist[0]
  std::size_t hist_n = 0;
  std::vector<std::uint8_t> chunk(block * bps);
  std::vector<float> cplx(2 * block);
  std::size_t carry = 0;

  std::fprintf(out,
               "time_s,sample_pos,icao,df,len,snr_db,freq_hz,sigma_hz,coherence,phase_rms_rad,samples,clipped,"
               "doppler_hz,range_km,range_rate_mps,pos_age_s,alt_ft,gs_kt,category\n");
  std::uint64_t n_frames = 0, n_est = 0, n_doppler = 0, n_lowcoh = 0;
  double busy_s = 0.0;

  auto handle = [&](std::span<const adsb::demod::DemodFrame> frames) {
    for (const auto& f : frames) {
      ++n_frames;
      const double t_s = f.sample_pos / fs;
      adsb::state::FrameMeta meta;
      meta.rx_ns = static_cast<std::uint64_t>(std::llround(std::max(0.0, t_s) * 1e9));
      meta.radio_ts = static_cast<std::uint64_t>(std::llround(std::max(0.0, f.sample_pos)));
      const auto fixes_before = engine.stats().cpr_global + engine.stats().cpr_local;
      engine.on_frame(f.bytes, f.len, meta, sink);
      if (engine.stats().cpr_global + engine.stats().cpr_local != fixes_before) {
        const auto t = tracks.find(f.icao);
        if (t != tracks.end() && t->second.has_pos) t->second.pos_ns = meta.rx_ns;
      }
      const adsb::demod::CarrierEstimate e = est.estimate(hist.data(), hist_base, hist_n, f);
      if (!e.ok) continue;
      ++n_est;
      if (e.coherence < 0.8) ++n_lowcoh;
      std::fprintf(out, "%.7f,%.3f,%06X,%d,%d,%.2f,%.2f,%.3f,%.5f,%.4f,%d,%d,", t_s, f.sample_pos, f.icao, f.df, f.len,
                   static_cast<double>(f.snr_db), e.freq_hz, e.sigma_hz, e.coherence, e.phase_rms_rad, e.samples,
                   e.clipped);
      const auto it = tracks.find(f.icao);
      bool wrote = false;
      if (have_rx && it != tracks.end() && it->second.has_pos) {
        const auto& u = it->second.u;
        const double age = (static_cast<double>(meta.rx_ns) - static_cast<double>(it->second.pos_ns)) / 1e9;
        const bool vel = u.gs_dkt != adsb::protocol::TrackUpdate::kGsDktNull &&
                         u.track_cdeg != adsb::protocol::TrackUpdate::kTrackCdegNull;
        if (vel && u.alt_ft != adsb::protocol::TrackUpdate::kAltFtNull && age <= max_age_s && age >= -1.0) {
          const double lat = u.lat_e7 * 1e-7, lon = u.lon_e7 * 1e-7, h = u.alt_ft * 0.3048;
          const double gs = u.gs_dkt * 0.1 * 1852.0 / 3600.0;
          const double trk = u.track_cdeg * 0.01 * adsb::pm::kPi / 180.0;
          const double vu =
              u.vrate_fpm == adsb::protocol::TrackUpdate::kVrateFpmNull ? 0.0 : u.vrate_fpm * 0.3048 / 60.0;
          double v[3], p[3];
          enu_to_ecef(lat, lon, gs * std::sin(trk), gs * std::cos(trk), vu, v);
          ecef(lat, lon, h, p);
          for (int k = 0; k < 3; ++k) p[k] += v[k] * age;  // to the message's time
          const double d[3] = {p[0] - rx_ecef[0], p[1] - rx_ecef[1], p[2] - rx_ecef[2]};
          const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
          const double rdot = (d[0] * v[0] + d[1] * v[1] + d[2] * v[2]) / std::max(r, 1.0);
          const double doppler = -(center_hz + e.freq_hz) * rdot / 299'792'458.0;
          std::fprintf(out, "%.2f,%.3f,%.3f,%.2f,%d,%.1f,%02X\n", doppler, r / 1000.0, rdot, age, u.alt_ft,
                       u.gs_dkt * 0.1, u.category);
          ++n_doppler;
          wrote = true;
        }
      }
      if (!wrote) {
        const int cat = it != tracks.end() ? it->second.u.category : 0;
        std::fprintf(out, ",,,,,,%02X\n", cat);
      }
    }
  };

  for (;;) {
    const std::size_t got = std::fread(chunk.data() + carry, 1, chunk.size() - carry, in);
    const std::size_t have = carry + got;
    const std::size_t n = have / bps;
    if (n > 0) {
      const auto t0 = std::chrono::steady_clock::now();
      adsb::demod::to_complex(chunk.data(), n, fmt, cplx.data());
      // Mark clipped samples before the DC offset moves them away from full
      // scale (the estimator leaves non-finite samples out).
      std::size_t usable = 0;
      double mi = 0.0, mq = 0.0;
      for (std::size_t k = 0; k < n; ++k) {
        const float vi = cplx[2 * k], vq = cplx[2 * k + 1];
        if (std::fabs(static_cast<double>(vi)) >= ccfg.clip_level ||
            std::fabs(static_cast<double>(vq)) >= ccfg.clip_level) {
          cplx[2 * k] = cplx[2 * k + 1] = std::numeric_limits<float>::quiet_NaN();
          continue;
        }
        mi += static_cast<double>(vi);
        mq += static_cast<double>(vq);
        ++usable;
      }
      if (dc && usable > 0) {
        const auto fi = static_cast<float>(mi / static_cast<double>(usable));
        const auto fq = static_cast<float>(mq / static_cast<double>(usable));
        for (std::size_t k = 0; k < n; ++k) {
          cplx[2 * k] -= fi;
          cplx[2 * k + 1] -= fq;
        }
      }
      // Append to the history, dropping the oldest samples beyond its capacity.
      if (hist_n + n > hist_cap) {
        const std::size_t drop = hist_n + n - hist_cap;
        std::memmove(hist.data(), hist.data() + 2 * drop, 2 * (hist_n - drop) * sizeof(float));
        hist_n -= drop;
        hist_base += static_cast<std::int64_t>(drop);
      }
      std::memcpy(hist.data() + 2 * hist_n, cplx.data(), 2 * n * sizeof(float));
      hist_n += n;
      handle(demod.feed(chunk.data(), n, fmt));
      busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    carry = have - n * bps;
    if (carry) std::memmove(chunk.data(), chunk.data() + n * bps, carry);
    if (got == 0) break;
  }
  handle(demod.flush());
  const bool read_failed = std::ferror(in) != 0;
  if (in != stdin) std::fclose(in);
  bool write_failed = std::ferror(out) != 0;
  if (out != stdout ? std::fclose(out) != 0 : std::fflush(out) != 0) write_failed = true;

  if (!quiet) {
    const auto samples = demod.stats().samples;
    std::fprintf(stderr,
                 "adsb-carrier: %llu samples (%.2f s at %.3f MS/s), %llu frames, %llu estimated, %llu with Doppler, "
                 "%llu with coherence < 0.8\n",
                 static_cast<unsigned long long>(samples), static_cast<double>(samples) / fs, fs / 1e6,
                 static_cast<unsigned long long>(n_frames), static_cast<unsigned long long>(n_est),
                 static_cast<unsigned long long>(n_doppler), static_cast<unsigned long long>(n_lowcoh));
    std::fprintf(stderr, "  throughput: %.1fx real time (one thread)\n",
                 busy_s > 0 ? static_cast<double>(samples) / fs / busy_s : 0.0);
  }
  if (read_failed) {
    std::fprintf(stderr, "adsb-carrier: read error on %s\n", data_path.c_str());
    return 1;
  }
  if (write_failed) {
    std::fprintf(stderr, "adsb-carrier: write error (output incomplete)\n");
    return 1;
  }
  return 0;
}
