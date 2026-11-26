// adsb-beast-import: convert a recorded Beast byte stream (a capture of
// dump1090-fa / readsb port 30005 written to disk) into an .adsbrec recording.
// Event time comes from the frames' 12 MHz counter (see beast_import.hpp), so
// every tool that reads .adsbrec (adsb-replay, adsb-publish, adsb-beast-serve)
// can replay the capture deterministically.
//
//   adsb-beast-import IN.beast|- -o OUT.adsbrec [--receiver LAT,LON[,ALT_M]]
//                     [--start 2026-11-19T15:36:00Z] [--from HOURS] [--hours HOURS]
//                     [--source NAME] [--receiver-id N] [--line N]
//
// A long capture can be imported in windows without unpacking it first:
//   tar -xOzf capture.tar.gz | adsb-beast-import - -o day2.adsbrec --from 24 --hours 24

#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "adsb/ingest/beast_import.hpp"
#include "adsb/record/recording.hpp"

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-beast-import IN.beast|- -o OUT.adsbrec [options]\n"
               "  IN                    Beast byte stream; '-' reads stdin (e.g. from tar -xOzf)\n"
               "  -o, --out FILE        output recording\n"
               "  --receiver LAT,LON[,ALT_M]  receiver position, stored in the header\n"
               "                        (adsb-replay uses it for local CPR decoding)\n"
               "  --start TIME          wall-clock time of the input's first frame, ISO 8601\n"
               "                        with an explicit zone: 2026-11-19T09:36:00-06:00 or ...Z\n"
               "                        (labelling only; the header gets the window's first frame)\n"
               "  --from HOURS          skip frames earlier than HOURS after the first frame\n"
               "  --hours HOURS         keep this many hours from --from (default: to the end)\n"
               "  --source NAME         source label, at most 16 characters (default beast)\n"
               "  --receiver-id N       receiver id in the header (default 0)\n"
               "  --line N              line id given to every record (default 0)\n"
               "  --reorder-ms MS       a step back of at most MS is reordering: the frame keeps\n"
               "                        the previous time (default 1000)\n"
               "  --reset-gap-ms MS     time inserted where the counter steps back further, i.e.\n"
               "                        the decoder restarted (default 1)\n");
  std::exit(code);
}

double num(const char* s, const char* flag) {
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || *end != '\0' || !std::isfinite(v)) {
    std::fprintf(stderr, "adsb-beast-import: bad value for %s: %s\n", flag, s);
    std::exit(1);
  }
  return v;
}

long long integer(const char* s, const char* flag, long long lo, long long hi) {
  const double v = num(s, flag);
  if (v != std::floor(v) || v < static_cast<double>(lo) || v > static_cast<double>(hi)) {
    std::fprintf(stderr, "adsb-beast-import: %s must be an integer in [%lld, %lld]\n", flag, lo, hi);
    std::exit(1);
  }
  return static_cast<long long>(v);
}

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's algorithm).
long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

// YYYY-MM-DDTHH:MM:SS followed by Z or +HH:MM / -HH:MM. A zone is required:
// the capture's file name alone does not say which one it is in.
bool parse_iso_utc_ns(const std::string& s, std::uint64_t* out) {
  int y, mo, d, h, mi, sec, n = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6) return false;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60 || h < 0 || mi < 0 ||
      sec < 0)
    return false;
  long long offset_s = 0;
  const char* z = s.c_str() + n;
  if (std::strcmp(z, "Z") == 0) {
    offset_s = 0;
  } else if ((z[0] == '+' || z[0] == '-') && std::strlen(z) == 6 && z[3] == ':') {
    int oh, om, m = 0;
    if (std::sscanf(z + 1, "%2d:%2d%n", &oh, &om, &m) != 2 || m != 5 || oh > 14 || om > 59) return false;
    offset_s = (oh * 3600LL + om * 60LL) * (z[0] == '+' ? 1 : -1);
  } else {
    return false;
  }
  const long long t = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400LL +
                      h * 3600LL + mi * 60LL + sec - offset_s;
  if (t < 0) return false;
  *out = static_cast<std::uint64_t>(t) * 1'000'000'000ULL;
  return true;
}

std::uint64_t hours_to_ns(double h) { return static_cast<std::uint64_t>(std::llround(h * 3600e9)); }

}  // namespace

int main(int argc, char** argv) {
  std::string in, out, start;
  adsb::ingest::BeastImportConfig cfg;
  adsb::record::RecordingInfo info;
  info.source = "beast";
  double from_h = 0, hours = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "-o" || a == "--out") out = val();
    else if (a == "--receiver") {
      const std::string v = val();
      double lat = 0, lon = 0, alt = 0;
      char end = 0;
      const int got = std::sscanf(v.c_str(), "%lf,%lf,%lf%c", &lat, &lon, &alt, &end);
      if ((got != 2 && got != 3) || !(std::fabs(lat) <= 90) || !(std::fabs(lon) <= 180) ||
          !(std::fabs(alt) < 1e5)) {
        std::fprintf(stderr, "adsb-beast-import: --receiver needs LAT,LON[,ALT_M] (degrees, metres)\n");
        return 1;
      }
      info.has_receiver_position = true;
      info.receiver_lat = lat;
      info.receiver_lon = lon;
      info.receiver_alt_m = static_cast<std::int32_t>(std::lround(alt));
    } else if (a == "--start") start = val();
    else if (a == "--from") from_h = num(val(), "--from");
    else if (a == "--hours") hours = num(val(), "--hours");
    else if (a == "--source") info.source = val();
    else if (a == "--receiver-id")
      info.receiver_id = static_cast<std::uint32_t>(integer(val(), "--receiver-id", 0, 0xFFFFFFFFLL));
    else if (a == "--line") cfg.line = static_cast<std::uint8_t>(integer(val(), "--line", 0, 255));
    else if (a == "--reorder-ms")
      cfg.reorder_ns = static_cast<std::uint64_t>(integer(val(), "--reorder-ms", 0, 86'400'000)) * 1'000'000;
    else if (a == "--reset-gap-ms")
      cfg.reset_gap_ns = static_cast<std::uint64_t>(integer(val(), "--reset-gap-ms", 0, 86'400'000)) * 1'000'000;
    else if (!a.empty() && a[0] == '-' && a != "-") {
      std::fprintf(stderr, "adsb-beast-import: unknown option %s\n", a.c_str());
      usage(1);
    } else if (in.empty()) in = a;
    else usage(1);
  }
  if (in.empty() || out.empty()) usage(1);
  if (info.source.size() > 16) {
    std::fprintf(stderr, "adsb-beast-import: --source is at most 16 characters\n");
    return 1;
  }
  if (!(from_h >= 0 && from_h < 1e5) || (hours != -1 && !(hours > 0 && hours < 1e5))) {
    std::fprintf(stderr, "adsb-beast-import: need 0 <= --from and 0 < --hours (in hours)\n");
    return 1;
  }
  std::uint64_t start_ns = 0;
  if (!start.empty() && !parse_iso_utc_ns(start, &start_ns)) {
    std::fprintf(stderr,
                 "adsb-beast-import: --start needs YYYY-MM-DDTHH:MM:SS with Z or +HH:MM/-HH:MM, got %s\n",
                 start.c_str());
    return 1;
  }
  cfg.from_ns = hours_to_ns(from_h);
  if (hours > 0) cfg.until_ns = cfg.from_ns + hours_to_ns(hours);

  std::FILE* f = in == "-" ? stdin : std::fopen(in.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "adsb-beast-import: cannot open %s: %s\n", in.c_str(), std::strerror(errno));
    return 1;
  }

  // The header holds the wall time of the first written record, known only
  // when it arrives: the output is created then (no file for an empty window).
  adsb::ingest::BeastImporter imp(cfg);
  adsb::record::RecordingWriter w;
  bool opened = false, write_failed = false;
  std::string err;
  auto sink = [&](const adsb::record::Record& r) {
    if (write_failed) return;
    if (!opened) {
      if (start_ns != 0) info.start_wall_ns = start_ns + (r.rx_ns - imp.stats().first_rx_ns);
      if (!w.open(out, info, &err)) {
        std::fprintf(stderr, "adsb-beast-import: %s\n", err.c_str());
        write_failed = true;
        return;
      }
      opened = true;
    }
    if (!w.append(r)) write_failed = true;
  };

  std::vector<std::uint8_t> buf(1 << 20);
  bool read_error = false;
  while (!imp.done() && !write_failed) {
    const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
    if (n > 0) imp.feed(buf.data(), n, sink);
    if (n < buf.size()) {
      read_error = std::ferror(f) != 0;
      break;
    }
  }
  if (f != stdin) std::fclose(f);
  if (read_error) {
    std::fprintf(stderr, "adsb-beast-import: read error on %s\n", in.c_str());
    return 1;
  }
  if (write_failed) {
    if (opened) std::fprintf(stderr, "adsb-beast-import: write failed: %s\n", out.c_str());
    return 1;
  }
  const auto& s = imp.stats();
  const auto& p = imp.parser_stats();
  if (!opened) {
    std::fprintf(stderr, "adsb-beast-import: no frames in the window (%" PRIu64 " frames read, %.2f h of input)\n",
                 s.frames, static_cast<double>(s.last_rx_ns - s.first_rx_ns) / 3600e9);
    return 1;
  }
  if (!w.close()) {
    std::fprintf(stderr, "adsb-beast-import: close failed: %s\n", out.c_str());
    return 1;
  }
  const double span = static_cast<double>(s.last_written_rx_ns - s.first_written_rx_ns) / 1e9;
  std::printf("wrote %s: %" PRIu64 " records over %.2f h (%.1f /s), from %.2f h after the first frame\n",
              out.c_str(), s.written, span / 3600, span > 0 ? static_cast<double>(s.written) / span : 0.0,
              static_cast<double>(s.first_written_rx_ns - s.first_rx_ns) / 3600e9);
  std::printf("  types: mode s long %" PRIu64 ", short %" PRIu64 ", mode a/c %" PRIu64
              "; status frames skipped %" PRIu64 "; outside the window %" PRIu64 "\n",
              s.mode_s_long, s.mode_s_short, s.mode_ac, s.status_skipped, s.outside_window);
  std::printf("  clock: out of order %" PRIu64 ", resets %" PRIu64 ", wraps %" PRIu64 ", gaps > 60 s %" PRIu64 ", longest gap %.3f s\n",
              s.out_of_order, s.clock_resets, s.clock_wraps, s.long_gaps, static_cast<double>(s.longest_gap_ns) / 1e9);
  std::printf("  parser: %" PRIu64 " bytes, bad type %" PRIu64 ", truncated %" PRIu64 ", skipped bytes %" PRIu64
              "%s\n",
              p.bytes, p.bad_type, p.truncated, p.skipped_bytes, imp.done() ? " (stopped at the window's end)" : "");
  return 0;
}
