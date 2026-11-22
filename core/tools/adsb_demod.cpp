// adsb-demod: demodulate a complex IQ recording into Mode S frames.
//
//   adsb-demod INPUT [options]
//
// INPUT is a SigMF recording (NAME.sigmf-data or NAME.sigmf-meta: the format
// and rate come from the metadata), a raw IQ file, or "-" for stdin (raw
// files and stdin need --format and --rate). Frames go out as Beast binary
// (12 MHz timestamps counted from the first sample, like readsb's ifile
// input) and/or readsb-style raw text; statistics and throughput go to
// stderr.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "adsb/demod/demod.hpp"

namespace {

using adsb::demod::DemodConfig;
using adsb::demod::Demodulator;
using adsb::demod::SampleFormat;

void usage() {
  std::fprintf(stderr,
               "usage: adsb-demod INPUT [options]\n"
               "  INPUT                 NAME.sigmf-data / NAME.sigmf-meta, a raw IQ file, or - (stdin)\n"
               "  --format F            cu8 | cs8 | cs16 | cf32 (raw input; SigMF gives it)\n"
               "  --rate HZ             sample rate (raw input; default 2.4e6)\n"
               "  --beast FILE|-        write frames as Beast binary\n"
               "  --raw FILE|-          write frames as text (*HEX;)\n"
               "  --fix                 correct one wrong bit in DF11/17/18 frames\n"
               "  --no-sequence         no sequence detector when the half-bit reading fails\n"
               "  --phases N            fractional starts tried per sample (default 5)\n"
               "  --magnitude M         exact | lut | ambm (default exact; lut is cu8 only)\n"
               "  --min-signal R        mean preamble pulse / noise floor, magnitude ratio (default 2.0)\n"
               "  --pulse-quiet R       each preamble pulse / quiet parts (default 1.2)\n"
               "  --gate R              first-stage gate, share of the weakest accepted pulse (default 0.9)\n"
               "  --resume-overlap US   after a frame, resume scanning US before its end (default 16)\n"
               "  --address-ttl S       Address/Parity frames need the address seen within S s (default 60)\n"
               "  --quiet               no statistics on stderr\n");
}

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// The value of "key": in a JSON text (first occurrence; strings unquoted).
bool json_value(const std::string& text, const std::string& key, std::string& out) {
  const std::size_t k = text.find("\"" + key + "\"");
  if (k == std::string::npos) return false;
  std::size_t i = text.find(':', k + key.size() + 2);  // after the key (which itself holds a colon)
  if (i == std::string::npos) return false;
  ++i;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\t')) ++i;
  if (i < text.size() && text[i] == '"') {
    const std::size_t e = text.find('"', i + 1);
    if (e == std::string::npos) return false;
    out = text.substr(i + 1, e - i - 1);
    return true;
  }
  std::size_t e = i;
  while (e < text.size() && text[e] != ',' && text[e] != '}' && text[e] != '\n') ++e;
  out = text.substr(i, e - i);
  return true;
}

FILE* open_out(const std::string& path) {
  if (path == "-") return stdout;
  return std::fopen(path.c_str(), "wb");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string input = argv[1];
  if (input == "-h" || input == "--help") {
    usage();
    return 0;
  }
  DemodConfig cfg;
  std::string format_arg, beast_path, raw_path;
  bool rate_given = false, quiet = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "adsb-demod: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    auto num = [&]() {
      const char* v = next();
      char* end = nullptr;
      const double d = std::strtod(v, &end);
      if (end == v || *end != '\0') {
        std::fprintf(stderr, "adsb-demod: bad number for %s: %s\n", a.c_str(), v);
        std::exit(2);
      }
      return d;
    };
    if (a == "--format") format_arg = next();
    else if (a == "--rate") { cfg.sample_rate_hz = num(); rate_given = true; }
    else if (a == "--beast") beast_path = next();
    else if (a == "--raw") raw_path = next();
    else if (a == "--fix") cfg.fix_single_bit = true;
    else if (a == "--no-sequence") cfg.sequence_detector = false;
    else if (a == "--phases") {
      const double v = num();
      if (!(v >= 1 && v <= 16) || v != std::floor(v)) {
        std::fprintf(stderr, "adsb-demod: --phases must be an integer in [1, 16]\n");
        return 2;
      }
      cfg.phases = static_cast<int>(v);
    }
    else if (a == "--magnitude") {
      const std::string m = next();
      if (m == "exact") cfg.magnitude = adsb::demod::Magnitude::kExact;
      else if (m == "lut") cfg.magnitude = adsb::demod::Magnitude::kLut;
      else if (m == "ambm") cfg.magnitude = adsb::demod::Magnitude::kAlphaMaxBetaMin;
      else { std::fprintf(stderr, "adsb-demod: --magnitude exact|lut|ambm\n"); return 2; }
    }
    else if (a == "--min-signal") cfg.min_signal_ratio = num();
    else if (a == "--pulse-quiet") cfg.min_pulse_quiet = num();
    else if (a == "--gate") cfg.gate_ratio = num();
    else if (a == "--resume-overlap") cfg.resume_overlap_us = num();
    else if (a == "--address-ttl") cfg.address_ttl_s = num();
    else if (a == "--quiet") quiet = true;
    else { std::fprintf(stderr, "adsb-demod: unknown option %s\n", a.c_str()); usage(); return 2; }
  }

  SampleFormat fmt = SampleFormat::kCu8;
  std::string data_path = input;
  if (ends_with(input, ".sigmf-meta") || ends_with(input, ".sigmf-data")) {
    const std::string base = input.substr(0, input.size() - 11);
    data_path = base + ".sigmf-data";
    std::ifstream mf(base + ".sigmf-meta");
    if (!mf) {
      std::fprintf(stderr, "adsb-demod: cannot read %s.sigmf-meta\n", base.c_str());
      return 1;
    }
    std::stringstream ss;
    ss << mf.rdbuf();
    const std::string meta = ss.str();
    std::string dt, rate;
    if (!json_value(meta, "core:datatype", dt) || !adsb::demod::parse_format(dt, fmt)) {
      std::fprintf(stderr, "adsb-demod: unsupported or missing core:datatype in the metadata\n");
      return 1;
    }
    if (!rate_given) {
      if (!json_value(meta, "core:sample_rate", rate)) {
        std::fprintf(stderr, "adsb-demod: no core:sample_rate in the metadata\n");
        return 1;
      }
      char* end = nullptr;
      cfg.sample_rate_hz = std::strtod(rate.c_str(), &end);
      if (end == rate.c_str()) {
        std::fprintf(stderr, "adsb-demod: bad core:sample_rate in the metadata: %s\n", rate.c_str());
        return 1;
      }
    }
    if (!format_arg.empty() && !adsb::demod::parse_format(format_arg, fmt)) {
      std::fprintf(stderr, "adsb-demod: unknown format %s\n", format_arg.c_str());
      return 2;
    }
  } else {
    if (format_arg.empty()) {
      std::fprintf(stderr, "adsb-demod: raw input needs --format\n");
      return 2;
    }
    if (!adsb::demod::parse_format(format_arg, fmt)) {
      std::fprintf(stderr, "adsb-demod: unknown format %s\n", format_arg.c_str());
      return 2;
    }
  }
  std::string why;
  if (!adsb::demod::validate(cfg, &why)) {
    std::fprintf(stderr, "adsb-demod: %s\n", why.c_str());
    return 2;
  }

  if (beast_path == "-" && raw_path == "-") {
    std::fprintf(stderr, "adsb-demod: --beast - and --raw - would mix binary and text on stdout\n");
    return 2;
  }
  FILE* in = data_path == "-" ? stdin : std::fopen(data_path.c_str(), "rb");
  if (!in) {
    std::fprintf(stderr, "adsb-demod: cannot open %s\n", data_path.c_str());
    return 1;
  }
  FILE* beast = nullptr;
  FILE* raw = nullptr;
  if (!beast_path.empty() && !(beast = open_out(beast_path))) {
    std::fprintf(stderr, "adsb-demod: cannot write %s\n", beast_path.c_str());
    return 1;
  }
  if (!raw_path.empty() && !(raw = open_out(raw_path))) {
    std::fprintf(stderr, "adsb-demod: cannot write %s\n", raw_path.c_str());
    return 1;
  }

  Demodulator demod(cfg);
  const std::size_t bps = adsb::demod::bytes_per_sample(fmt);
  std::vector<std::uint8_t> chunk(cfg.block_samples * bps);
  std::vector<std::uint8_t> beast_buf;
  std::size_t carry = 0;  // bytes of a partial sample from the previous read
  double busy_s = 0.0;
  bool write_failed = false;
  auto emit = [&](std::span<const adsb::demod::DemodFrame> frames) {
    for (const auto& f : frames) {
      if (beast) adsb::demod::to_beast(f, cfg.sample_rate_hz, beast_buf);
      if (raw) {
        std::fputc('*', raw);
        for (int i = 0; i < f.len; ++i) std::fprintf(raw, "%02X", f.bytes[i]);
        std::fputs(";\n", raw);
      }
    }
    if (beast && !beast_buf.empty()) {
      if (std::fwrite(beast_buf.data(), 1, beast_buf.size(), beast) != beast_buf.size()) write_failed = true;
      beast_buf.clear();
    }
  };
  for (;;) {
    const std::size_t got = std::fread(chunk.data() + carry, 1, chunk.size() - carry, in);
    const std::size_t have = carry + got;
    const std::size_t n = have / bps;
    if (n > 0) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto frames = demod.feed(chunk.data(), n, fmt);
      busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      emit(frames);
    }
    carry = have - n * bps;
    if (carry) std::memmove(chunk.data(), chunk.data() + n * bps, carry);
    if (got == 0) break;
  }
  {
    const auto t0 = std::chrono::steady_clock::now();
    const auto frames = demod.flush();
    busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    emit(frames);
  }
  const bool read_failed = std::ferror(in) != 0;
  if (in != stdin) std::fclose(in);
  if (raw && std::ferror(raw)) write_failed = true;
  if (beast && beast != stdout && std::fclose(beast) != 0) write_failed = true;
  if (raw && raw != stdout && std::fclose(raw) != 0) write_failed = true;
  if ((beast == stdout || raw == stdout) && std::fflush(stdout) != 0) write_failed = true;
  if (carry) std::fprintf(stderr, "adsb-demod: warning: %zu trailing bytes are not a whole sample (ignored)\n", carry);

  if (!quiet) {
    const auto& s = demod.stats();
    std::fprintf(stderr, "adsb-demod: %llu samples (%.2f s at %.3f MS/s, %s), %llu frames",
                 static_cast<unsigned long long>(s.samples), static_cast<double>(s.samples) / cfg.sample_rate_hz,
                 cfg.sample_rate_hz / 1e6, adsb::demod::format_name(fmt), static_cast<unsigned long long>(s.frames));
    std::fprintf(stderr, ", %llu of them by the sequence detector", static_cast<unsigned long long>(s.sequence_decoded));
    if (cfg.fix_single_bit) std::fprintf(stderr, " (%llu with one bit fixed)", static_cast<unsigned long long>(s.corrected));
    std::fprintf(stderr, "\n  by format:");
    for (int df = 0; df < 32; ++df) {
      if (s.by_df[df]) std::fprintf(stderr, " DF%d %llu", df, static_cast<unsigned long long>(s.by_df[df]));
    }
    std::fprintf(stderr, "\n  noise floor %.5f of full scale (%.1f dBFS)", static_cast<double>(demod.noise_floor()),
                 demod.noise_floor() > 0 ? 20.0 * std::log10(static_cast<double>(demod.noise_floor())) : -999.0);
    std::fprintf(stderr, "\n  gated %llu positions, %llu preambles, %llu failed the check (%llu unknown address)\n",
                 static_cast<unsigned long long>(s.gated), static_cast<unsigned long long>(s.preambles),
                 static_cast<unsigned long long>(s.bad_crc), static_cast<unsigned long long>(s.unknown_address));
    const double ns = s.samples ? busy_s * 1e9 / static_cast<double>(s.samples) : 0.0;
    std::fprintf(stderr, "  throughput: %.2f ns/sample (%.1f MS/s, %.1fx real time; magnitude %s, one thread)\n", ns,
                 ns > 0 ? 1e3 / ns : 0.0, ns > 0 ? 1e9 / ns / cfg.sample_rate_hz : 0.0,
                 adsb::demod::magnitude_name(cfg.magnitude));
  }
  if (read_failed) {
    std::fprintf(stderr, "adsb-demod: read error on %s\n", data_path.c_str());
    return 1;
  }
  if (write_failed) {
    std::fprintf(stderr, "adsb-demod: write error (output incomplete)\n");
    return 1;
  }
  return 0;
}
