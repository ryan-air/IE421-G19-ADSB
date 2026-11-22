// adsb-iqsynth: synthetic 1090 MHz IQ recordings with per-message truth
// (README_UPDATES.md §22.4). Traffic comes from adsb-synth's generator; the
// IQ model is in core/include/adsb/sim/iq.hpp.
//
//   adsb-iqsynth -o scenario [--format cu8] [--rate 2.4e6] [--aircraft 50]
//                [--duration 10] ...
//
// writes scenario.sigmf-data (samples) and scenario.sigmf-meta (SigMF JSON:
// configuration, every transmitter, one annotation per message). `--data -`
// streams samples to stdout instead, e.g. into readsb --ifile -.
//
// Never transmit the output over the air: playing it into a receiver is
// only legal by cable through attenuators or in a shielded enclosure.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-iqsynth -o BASE [options]      (writes BASE.sigmf-data + BASE.sigmf-meta)\n"
               "       adsb-iqsynth --data FILE|- [--meta FILE] [options]\n"
               "traffic:\n"
               "  --aircraft N          aircraft (default 50)\n"
               "  --duration S          seconds (default 10)\n"
               "  --seed N              traffic seed (default 1)\n"
               "  --iq-seed N           noise / phase / transmitter seed (default: --seed)\n"
               "  --receiver LAT,LON    receiver (default Willard Airport, CMI)\n"
               "  --max-range NM        aircraft spawn radius (default 150)\n"
               "  --ground F            share of aircraft on the ground (default 0.06)\n"
               "  --surveillance HZ     DF4/DF5 replies per aircraft per second (default 0.3)\n"
               "  --no-df11             no DF11 acquisition squitters\n"
               "sampling:\n"
               "  --format F            cu8 (RTL-SDR, readsb UC8), cs8, cs16 (readsb SC16), cf32 (default cu8)\n"
               "  --rate HZ             sample rate (default 2.4e6)\n"
               "  --oversample K        points averaged per sample (default 8)\n"
               "  --threads N           render threads (default: hardware; output is the same)\n"
               "receiver:\n"
               "  --noise-figure DB     (default 5)\n"
               "  --full-scale DBM      antenna-port power at ADC full scale (default -70; lower = more gain)\n"
               "  --ppm X               LO and sample-clock error (default 0)\n"
               "  --antenna-height M    above ground (default 10)\n"
               "  --omni                isotropic antenna instead of a vertical dipole\n"
               "  --dc I,Q              DC offset, fraction of full scale\n"
               "  --iq-imbalance G,DEG  Q gain error and phase error\n"
               "  --no-noise            no thermal noise\n"
               "transmitters:\n"
               "  --tx-offset-sd HZ     per-aircraft carrier offset spread (default 100e3; clamped to +-1 MHz)\n"
               "  --tx-drift-sd HZ/S    per-aircraft drift spread (default 0.5)\n"
               "  --tx-jitter-sd HZ     per-message jitter (default 20)\n"
               "  --tx-power MIN,MAX    dBm (default 51,57: 125-500 W)\n"
               "channel:\n"
               "  --no-doppler\n"
               "  --k-factor K          effective Earth radius factor (default 1.333; larger = ducting)\n"
               "  --diffraction DB/KM   loss beyond the radio horizon (default 1)\n"
               "  --ground-reflection G two-ray |Gamma| (default 0.3; 0 = off)\n"
               "  --reflector LAT,LON,ALT_M,LOSS_DB   fixed reflector (repeatable, up to 6)\n"
               "  --min-snr DB          skip messages below this SNR (default -15)\n"
               "  --tail S              noise-only seconds after the traffic (default 0)\n"
               "output:\n"
               "  -o BASE | --data FILE|- | --meta FILE | --no-meta\n");
  std::exit(code);
}

double num(const char* s, const char* flag) {
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || *end != '\0' || !std::isfinite(v)) {
    std::fprintf(stderr, "adsb-iqsynth: bad value for %s: %s\n", flag, s);
    std::exit(1);
  }
  return v;
}

long long integer(const char* s, const char* flag, long long lo, long long hi) {
  const double v = num(s, flag);
  if (v != std::floor(v) || v < static_cast<double>(lo) || v > static_cast<double>(hi)) {
    std::fprintf(stderr, "adsb-iqsynth: %s must be an integer in [%lld, %lld]\n", flag, lo, hi);
    std::exit(1);
  }
  return static_cast<long long>(v);
}

std::vector<double> nums(const std::string& v, const char* flag, std::size_t count) {
  std::vector<double> out;
  std::size_t pos = 0;
  while (pos <= v.size()) {
    const auto comma = v.find(',', pos);
    const std::string part = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    out.push_back(num(part.c_str(), flag));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  if (out.size() != count) {
    std::fprintf(stderr, "adsb-iqsynth: %s takes %zu comma-separated values\n", flag, count);
    std::exit(1);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  adsb::sim::SynthConfig syn;
  syn.duration_s = 10.0;
  syn.keep_transmissions = true;
  adsb::sim::IqConfig cfg;
  std::string base, data_path, meta_path;
  bool no_meta = false, iq_seed_set = false;
  unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 64u);

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "-o" || a == "--out") base = val();
    else if (a == "--data") data_path = val();
    else if (a == "--meta") meta_path = val();
    else if (a == "--no-meta") no_meta = true;
    else if (a == "--aircraft") syn.aircraft = static_cast<int>(integer(val(), a.c_str(), 1, 100000));
    else if (a == "--duration") syn.duration_s = num(val(), a.c_str());
    else if (a == "--seed") syn.seed = static_cast<std::uint64_t>(integer(val(), a.c_str(), 0, 1LL << 53));
    else if (a == "--iq-seed") {
      cfg.seed = static_cast<std::uint64_t>(integer(val(), a.c_str(), 0, 1LL << 53));
      iq_seed_set = true;
    } else if (a == "--receiver") {
      const auto v = nums(val(), "--receiver", 2);
      syn.receiver_lat = v[0];
      syn.receiver_lon = v[1];
    } else if (a == "--max-range") syn.max_range_nm = num(val(), a.c_str());
    else if (a == "--ground") syn.ground_fraction = num(val(), a.c_str());
    else if (a == "--surveillance") syn.surveillance_rate_hz = num(val(), a.c_str());
    else if (a == "--no-df11") syn.df11_squitters = false;
    else if (a == "--format") {
      const auto f = adsb::sim::parse_iq_format(val());
      if (!f) {
        std::fprintf(stderr, "adsb-iqsynth: --format must be cu8, cs8, cs16 or cf32\n");
        return 1;
      }
      cfg.format = *f;
    } else if (a == "--rate") cfg.sample_rate_hz = num(val(), a.c_str());
    else if (a == "--oversample") cfg.oversample = static_cast<int>(integer(val(), a.c_str(), 1, 64));
    else if (a == "--threads") threads = static_cast<unsigned>(integer(val(), a.c_str(), 1, 64));
    else if (a == "--noise-figure") cfg.noise_figure_db = num(val(), a.c_str());
    else if (a == "--full-scale") cfg.full_scale_dbm = num(val(), a.c_str());
    else if (a == "--ppm") cfg.rx_ppm = num(val(), a.c_str());
    else if (a == "--antenna-height") cfg.antenna_height_m = num(val(), a.c_str());
    else if (a == "--omni") cfg.antenna = adsb::sim::AntennaPattern::kOmni;
    else if (a == "--dc") {
      const auto v = nums(val(), "--dc", 2);
      cfg.dc_i = v[0];
      cfg.dc_q = v[1];
    } else if (a == "--iq-imbalance") {
      const auto v = nums(val(), "--iq-imbalance", 2);
      cfg.iq_gain_imbalance = v[0];
      cfg.iq_phase_deg = v[1];
    } else if (a == "--no-noise") cfg.noise = false;
    else if (a == "--tx-offset-sd") cfg.tx_offset_sd_hz = num(val(), a.c_str());
    else if (a == "--tx-drift-sd") cfg.tx_drift_sd_hz_per_s = num(val(), a.c_str());
    else if (a == "--tx-jitter-sd") cfg.tx_jitter_sd_hz = num(val(), a.c_str());
    else if (a == "--tx-power") {
      const auto v = nums(val(), "--tx-power", 2);
      cfg.tx_power_min_dbm = v[0];
      cfg.tx_power_max_dbm = v[1];
    } else if (a == "--no-doppler") cfg.doppler = false;
    else if (a == "--k-factor") cfg.k_factor = num(val(), a.c_str());
    else if (a == "--diffraction") cfg.diffraction_db_per_km = num(val(), a.c_str());
    else if (a == "--ground-reflection") cfg.ground_reflection = num(val(), a.c_str());
    else if (a == "--reflector") {
      const auto v = nums(val(), "--reflector", 4);
      if (cfg.reflectors.size() >= adsb::sim::kMaxIqPaths - 2) {
        std::fprintf(stderr, "adsb-iqsynth: at most %d reflectors\n", adsb::sim::kMaxIqPaths - 2);
        return 1;
      }
      cfg.reflectors.push_back({v[0], v[1], v[2], v[3]});
    } else if (a == "--min-snr") cfg.min_snr_db = num(val(), a.c_str());
    else if (a == "--tail") cfg.tail_s = num(val(), a.c_str());
    else {
      std::fprintf(stderr, "adsb-iqsynth: unknown option %s\n", a.c_str());
      usage(1);
    }
  }
  if (!iq_seed_set) cfg.seed = syn.seed;
  if (!base.empty()) {
    if (data_path.empty()) data_path = base + ".sigmf-data";
    if (meta_path.empty()) meta_path = base + ".sigmf-meta";
  }
  if (data_path.empty()) usage(1);
  if (no_meta) meta_path.clear();
  std::string why;
  auto traffic_ok = [&](bool ok, const char* msg) {
    if (!ok && why.empty()) why = msg;
    return ok;
  };
  const bool ok_traffic =
      traffic_ok(std::isfinite(syn.duration_s) && syn.duration_s > 0 && syn.duration_s <= 3600,
                 "--duration must be in (0, 3600]") &&
      traffic_ok(syn.max_range_nm > 0 && syn.max_range_nm <= 1000, "--max-range must be in (0, 1000]") &&
      traffic_ok(syn.ground_fraction >= 0 && syn.ground_fraction <= 1, "--ground must be in [0, 1]") &&
      traffic_ok(syn.surveillance_rate_hz >= 0 && syn.surveillance_rate_hz <= 100,
                 "--surveillance must be in [0, 100]") &&
      traffic_ok(syn.receiver_lat >= -89 && syn.receiver_lat <= 89 && syn.receiver_lon >= -180 &&
                     syn.receiver_lon <= 180,
                 "--receiver must be a latitude in [-89, 89] and a longitude in [-180, 180]") &&
      traffic_ok(threads <= 64, "--threads must be at most 64");
  if (!ok_traffic || !adsb::sim::validate_iq_config(cfg, &why)) {
    std::fprintf(stderr, "adsb-iqsynth: %s\n", why.c_str());
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const adsb::sim::SynthOutput traffic = adsb::sim::generate(syn);
  const adsb::sim::IqPlan plan = adsb::sim::plan_iq(syn, traffic, cfg);

  if (!meta_path.empty()) {
    std::FILE* m = std::fopen(meta_path.c_str(), "w");
    const std::string meta = adsb::sim::sigmf_meta(syn, cfg, plan);
    if (!m || std::fwrite(meta.data(), 1, meta.size(), m) != meta.size() || std::fclose(m) != 0) {
      std::fprintf(stderr, "adsb-iqsynth: cannot write %s\n", meta_path.c_str());
      return 1;
    }
  }

  const bool to_stdout = data_path == "-";
  std::FILE* out = to_stdout ? stdout : std::fopen(data_path.c_str(), "wb");
  if (!out) {
    std::fprintf(stderr, "adsb-iqsynth: cannot create %s\n", data_path.c_str());
    return 1;
  }
  std::FILE* log = to_stdout ? stderr : stdout;

  // Render in batches of `threads` chunks, write them in order.
  const std::uint64_t chunks = (plan.total_samples + adsb::sim::kIqChunkSamples - 1) / adsb::sim::kIqChunkSamples;
  const std::size_t bps = adsb::sim::iq_bytes_per_sample(cfg.format);
  const std::size_t batch = threads;
  std::vector<std::vector<float>> fbuf(batch, std::vector<float>(2 * adsb::sim::kIqChunkSamples));
  std::vector<std::vector<std::uint8_t>> qbuf(batch, std::vector<std::uint8_t>(bps * adsb::sim::kIqChunkSamples));
  std::vector<std::size_t> got(batch);
  std::vector<std::uint64_t> clipped(batch);
  std::uint64_t clipped_total = 0, written = 0;
  for (std::uint64_t c0 = 0; c0 < chunks; c0 += batch) {
    const std::size_t nb = static_cast<std::size_t>(std::min<std::uint64_t>(batch, chunks - c0));
    auto work = [&](std::size_t b) {
      got[b] = adsb::sim::render_iq_chunk(plan, cfg, c0 + b, fbuf[b].data());
      clipped[b] = adsb::sim::quantize_iq(fbuf[b].data(), got[b], cfg.format, qbuf[b].data());
    };
    if (nb == 1) {
      work(0);
    } else {
      std::vector<std::thread> pool;
      for (std::size_t b = 0; b < nb; ++b) pool.emplace_back(work, b);
      for (auto& t : pool) t.join();
    }
    for (std::size_t b = 0; b < nb; ++b) {
      const std::size_t bytes = got[b] * bps;
      if (std::fwrite(qbuf[b].data(), 1, bytes, out) != bytes) {
        std::fprintf(stderr, "adsb-iqsynth: write failed (%s)\n", data_path.c_str());
        return 1;
      }
      written += got[b];
      clipped_total += clipped[b];
    }
  }
  if (to_stdout ? std::fflush(out) != 0 : std::fclose(out) != 0) {
    std::fprintf(stderr, "adsb-iqsynth: write failed (%s)\n", data_path.c_str());
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  // SNR distribution of the rendered messages.
  std::uint64_t bins[5] = {};  // <0, 0-10, 10-20, 20-30, >=30 dB
  for (const auto& m : plan.messages) {
    const int b = m.snr_db < 0 ? 0 : m.snr_db < 10 ? 1 : m.snr_db < 20 ? 2 : m.snr_db < 30 ? 3 : 4;
    ++bins[b];
  }
  std::fprintf(log, "wrote %s: %llu samples (%s, %.3f MS/s, %.2f s) from %d aircraft\n",
               to_stdout ? "stdout" : data_path.c_str(), static_cast<unsigned long long>(written),
               adsb::sim::iq_format_name(cfg.format), cfg.sample_rate_hz / 1e6, syn.duration_s, syn.aircraft);
  if (!meta_path.empty()) std::fprintf(log, "  metadata %s\n", meta_path.c_str());
  std::fprintf(log,
               "  messages: %llu transmitted, %llu rendered, %llu below %.0f dB SNR, %llu collided (%.1f%%)\n",
               static_cast<unsigned long long>(plan.stats.transmissions),
               static_cast<unsigned long long>(plan.stats.rendered),
               static_cast<unsigned long long>(plan.stats.too_weak), cfg.min_snr_db,
               static_cast<unsigned long long>(plan.stats.collided),
               plan.stats.rendered ? 100.0 * static_cast<double>(plan.stats.collided) /
                                         static_cast<double>(plan.stats.rendered)
                                   : 0.0);
  std::fprintf(log, "  SNR: <0 dB %llu, 0-10 %llu, 10-20 %llu, 20-30 %llu, >=30 %llu\n",
               static_cast<unsigned long long>(bins[0]), static_cast<unsigned long long>(bins[1]),
               static_cast<unsigned long long>(bins[2]), static_cast<unsigned long long>(bins[3]),
               static_cast<unsigned long long>(bins[4]));
  std::fprintf(log, "  noise %.1f dBm (sigma %.4f of full scale), clipped samples %llu\n", plan.noise_dbm,
               plan.noise_sigma, static_cast<unsigned long long>(clipped_total));
  std::fprintf(log, "  %.2f s (%.1f MS/s with %u threads)\n", secs,
               secs > 0 ? static_cast<double>(written) / secs / 1e6 : 0.0, threads);
  return 0;
}
