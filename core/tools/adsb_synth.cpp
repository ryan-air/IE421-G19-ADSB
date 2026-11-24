// adsb-synth: generate synthetic ADS-B traffic as an .adsbrec recording
// (and optionally a raw Beast stream and a ground-truth CSV).
//
//   adsb-synth -o traffic.adsbrec [--aircraft 50] [--duration 60] [--seed 1]
//              [--lines 1|2] [--max-range 150] [--corrupt 0.003]
//              [--receiver LAT,LON] [--beast out.beast] [--truth truth.csv]
//              [--avionics [--airspeed-share 0.3]]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "adsb/ingest/beast.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/sim/synth.hpp"

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-synth -o FILE.adsbrec [options]\n"
               "  --aircraft N        number of aircraft (default 50)\n"
               "  --duration SECONDS  simulated time (default 60)\n"
               "  --seed N            RNG seed (default 1); same seed = same output\n"
               "  --lines 1|2         receiver lines, 2 = A/B redundant receivers (default 1)\n"
               "  --line-b-delay US   line B's extra latency over line A (default 250; negative = B faster)\n"
               "  --outage L:FROM:TO  line L (0 or 1) receives nothing transmitted from FROM to TO seconds\n"
               "                      (an unplugged receiver; repeat for the other line)\n"
               "  --max-range NM      reception range (default 150)\n"
               "  --corrupt P         share of frames with a flipped bit (default 0.003)\n"
               "  --ground F          share of aircraft taxiing at the airport (default 0.06)\n"
               "  --receiver LAT,LON  receiver position (default Willard Airport, CMI)\n"
               "  --avionics          mixed ADS-B versions, position type codes / NIC and\n"
               "                      operational status (TC 31); some aircraft send\n"
               "                      airspeed + heading instead of ground speed\n"
               "  --airspeed-share F  share of airborne aircraft sending airspeed (default 0.3)\n"
               "  --beast FILE        also write line 0 as a raw Beast byte stream\n"
               "  --truth FILE        also write ground truth (CSV) for every position frame\n");
  std::exit(code);
}

double num(const char* s, const char* flag) {
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || *end != '\0' || !std::isfinite(v)) {
    std::fprintf(stderr, "adsb-synth: bad value for %s: %s\n", flag, s);
    std::exit(1);
  }
  return v;
}

// Integer option within [lo, hi]; rejects fractions and out-of-range values.
long long integer(const char* s, const char* flag, long long lo, long long hi) {
  const double v = num(s, flag);
  if (v != std::floor(v) || v < static_cast<double>(lo) || v > static_cast<double>(hi)) {
    std::fprintf(stderr, "adsb-synth: %s must be an integer in [%lld, %lld]\n", flag, lo, hi);
    std::exit(1);
  }
  return static_cast<long long>(v);
}

}  // namespace

int main(int argc, char** argv) {
  adsb::sim::SynthConfig cfg;
  std::string out, beast_path, truth_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "-o" || a == "--out") out = val();
    else if (a == "--aircraft") cfg.aircraft = static_cast<int>(integer(val(), "--aircraft", 1, 100000));
    else if (a == "--duration") cfg.duration_s = num(val(), "--duration");
    else if (a == "--seed") cfg.seed = static_cast<std::uint64_t>(integer(val(), "--seed", 0, 1LL << 53));
    else if (a == "--lines") cfg.lines = static_cast<int>(integer(val(), "--lines", 1, 2));
    else if (a == "--line-b-delay") cfg.line_b_extra_latency_us = num(val(), "--line-b-delay");
    else if (a == "--outage") {
      int line = -1;
      double from = 0, to = 0;
      char end = 0;
      if (std::sscanf(val(), "%d:%lf:%lf%c", &line, &from, &to, &end) != 3 || line < 0 || line > 1 ||
          !(to > from)) {
        std::fprintf(stderr, "adsb-synth: --outage needs LINE:FROM:TO with LINE 0 or 1 and TO > FROM\n");
        return 1;
      }
      cfg.outage[line].start_s = from;
      cfg.outage[line].end_s = to;
    } else if (a == "--max-range") cfg.max_range_nm = num(val(), "--max-range");
    else if (a == "--corrupt") cfg.corrupt_rate = num(val(), "--corrupt");
    else if (a == "--ground") cfg.ground_fraction = num(val(), "--ground");
    else if (a == "--receiver") {
      const std::string v = val();
      const auto comma = v.find(',');
      if (comma == std::string::npos) usage(1);
      cfg.receiver_lat = num(v.substr(0, comma).c_str(), "--receiver");
      cfg.receiver_lon = num(v.substr(comma + 1).c_str(), "--receiver");
    } else if (a == "--avionics") cfg.avionics = true;
    else if (a == "--airspeed-share") {
      cfg.airspeed_fraction = num(val(), "--airspeed-share");
      if (cfg.airspeed_fraction < 0 || cfg.airspeed_fraction > 1) {
        std::fprintf(stderr, "adsb-synth: --airspeed-share must be in [0, 1]\n");
        return 1;
      }
    } else if (a == "--beast") beast_path = val();
    else if (a == "--truth") truth_path = val();
    else {
      std::fprintf(stderr, "adsb-synth: unknown option %s\n", a.c_str());
      usage(1);
    }
  }
  if (out.empty()) usage(1);
  if (cfg.rx_latency_us + cfg.line_b_extra_latency_us < 0) {
    std::fprintf(stderr, "adsb-synth: --line-b-delay below -%g us would make line B receive before transmission\n",
                 cfg.rx_latency_us);
    return 1;
  }
  if (cfg.aircraft < 1 || cfg.duration_s <= 0 || cfg.lines < 1 || cfg.lines > 2) {
    std::fprintf(stderr, "adsb-synth: need --aircraft >= 1, --duration > 0, --lines 1 or 2\n");
    return 1;
  }

  const adsb::sim::SynthOutput s = adsb::sim::generate(cfg);

  adsb::record::RecordingWriter w;
  std::string err;
  if (!w.open(out, adsb::sim::recording_info(cfg), &err)) {
    std::fprintf(stderr, "adsb-synth: %s\n", err.c_str());
    return 1;
  }
  for (const auto& r : s.records) {
    if (!w.append(r)) {
      std::fprintf(stderr, "adsb-synth: write failed\n");
      return 1;
    }
  }
  if (!w.close()) {
    std::fprintf(stderr, "adsb-synth: close failed\n");
    return 1;
  }

  if (!beast_path.empty()) {
    std::FILE* f = std::fopen(beast_path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "adsb-synth: cannot create %s\n", beast_path.c_str());
      return 1;
    }
    for (const auto& r : s.records) {
      if (r.line != 0) continue;
      adsb::ingest::BeastFrame bf;
      bf.type = static_cast<adsb::ingest::BeastType>(r.type);
      bf.len = r.len;
      bf.signal = r.signal;
      bf.timestamp = r.radio_ts & 0xFFFFFFFFFFFFULL;
      std::memcpy(bf.payload, r.payload, r.len);
      std::uint8_t buf[adsb::ingest::kBeastMaxEncodedLen];
      const std::size_t n = adsb::ingest::beast_encode(bf, buf);
      if (std::fwrite(buf, 1, n, f) != n) {
        std::fprintf(stderr, "adsb-synth: write failed: %s\n", beast_path.c_str());
        std::fclose(f);
        return 1;
      }
    }
    if (std::fclose(f) != 0) {
      std::fprintf(stderr, "adsb-synth: write failed: %s\n", beast_path.c_str());
      return 1;
    }
  }

  if (!truth_path.empty()) {
    std::FILE* f = std::fopen(truth_path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "adsb-synth: cannot create %s\n", truth_path.c_str());
      return 1;
    }
    std::fprintf(f, "icao,tx_ns,lat,lon,alt_ft,gs_kt,track_deg,vrate_fpm,on_ground\n");
    for (const auto& t : s.truth) {
      std::fprintf(f, "%06X,%llu,%.7f,%.7f,%d,%.1f,%.2f,%d,%d\n", t.icao,
                   static_cast<unsigned long long>(t.tx_ns), t.lat, t.lon, t.alt_ft,
                   t.ground_speed_kt, t.track_deg, t.vertical_rate_fpm, t.on_ground ? 1 : 0);
    }
    std::fclose(f);
  }

  std::printf("wrote %s: %zu records from %d aircraft over %.0f s (%d line%s)\n", out.c_str(),
              s.records.size(), cfg.aircraft, cfg.duration_s, cfg.lines, cfg.lines > 1 ? "s" : "");
  std::printf("  transmitted %llu, received %llu, lost %llu, corrupted %llu, outage %llu\n",
              static_cast<unsigned long long>(s.stats.transmitted),
              static_cast<unsigned long long>(s.stats.received),
              static_cast<unsigned long long>(s.stats.lost),
              static_cast<unsigned long long>(s.stats.corrupted),
              static_cast<unsigned long long>(s.stats.outage));
  return 0;
}
