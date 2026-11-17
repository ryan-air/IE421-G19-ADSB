// adsb-replay: replay an .adsbrec recording through decode + track state.
//
//   adsb-replay FILE.adsbrec [--speed max|N] [--pipeline] [--cpus R,W]
//               [--out updates.bin] [--no-flush] [--allow-truncated]
//
// Prints the record and engine counters and the SHA-256 of the emitted
// TrackUpdate/TrackDelete stream. The hash is identical for every --speed and
// with or without --pipeline (the determinism contract, README_UPDATES §11.2);
// CI checks this. With --pipeline it also prints per-stage latency percentiles.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "adsb/common/build_config.hpp"
#include "adsb/feed/book.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/replay/replay.hpp"

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-replay FILE.adsbrec [options]\n"
               "  --speed max|N     max (default) or N x original speed (1 = real time)\n"
               "  --pipeline        two threads: reader -> queue -> decode+state, with latency stats\n"
               "  --busy-wait       pace by spinning instead of sleeping (precise timing, uses a core)\n"
               "  --cpus R,W        pin reader and worker threads (Linux only; implies --pipeline)\n"
               "  --out FILE        write the emitted messages as {u16 len, bytes} records\n"
               "  --no-flush        do not expire the remaining tracks after the last record\n"
               "  --allow-truncated read a recording that has no footer (recorder was killed)\n"
               "  --hash-only       print only the output hash\n"
               "  --book-digest     also print the digest of the final track book (what a feed\n"
               "                    subscriber reconstructs; compare with adsb-publish --no-publish-ts)\n");
  std::exit(code);
}

void print_hist(const char* name, const adsb::LatencyHistogram& h) {
  std::printf("  %-14s p50 %8llu  p99 %8llu  p99.9 %8llu  p99.99 %8llu  max %10llu ns\n", name,
              static_cast<unsigned long long>(h.percentile(50)),
              static_cast<unsigned long long>(h.percentile(99)),
              static_cast<unsigned long long>(h.percentile(99.9)),
              static_cast<unsigned long long>(h.percentile(99.99)),
              static_cast<unsigned long long>(h.max()));
}

}  // namespace

int main(int argc, char** argv) {
  std::string path, out_path;
  adsb::replay::ReplayOptions opt;
  opt.flush_at_end = true;
  bool pipeline = false, allow_truncated = false, hash_only = false, book_digest = false;
  int reader_cpu = -1, worker_cpu = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--speed") {
      const std::string v = val();
      opt.speed = v == "max" ? 0.0 : std::strtod(v.c_str(), nullptr);
      if (v != "max" && opt.speed <= 0.0) usage(1);
    } else if (a == "--pipeline") pipeline = true;
    else if (a == "--busy-wait") opt.busy_wait = true;
    else if (a == "--cpus") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%d,%d", &reader_cpu, &worker_cpu) != 2) usage(1);
      pipeline = true;
    } else if (a == "--out") out_path = val();
    else if (a == "--no-flush") opt.flush_at_end = false;
    else if (a == "--allow-truncated") allow_truncated = true;
    else if (a == "--hash-only") hash_only = true;
    else if (a == "--book-digest") book_digest = true;
    else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "adsb-replay: unknown option %s\n", a.c_str());
      usage(1);
    } else if (path.empty()) path = a;
    else usage(1);
  }
  if (path.empty()) usage(1);

  adsb::record::RecordingReader rd;
  std::string err;
  if (!rd.open(path, &err, allow_truncated)) {
    std::fprintf(stderr, "adsb-replay: %s\n", err.c_str());
    return 1;
  }

  adsb::state::EngineConfig ecfg;
  if (rd.info().has_receiver_position) {
    ecfg.has_receiver = true;
    ecfg.receiver_lat = rd.info().receiver_lat;
    ecfg.receiver_lon = rd.info().receiver_lon;
  }
  auto engine = std::make_unique<adsb::DefaultTrackEngine>(ecfg);

  std::vector<std::uint8_t> captured;
  std::vector<std::uint8_t>* capture = out_path.empty() && !book_digest ? nullptr : &captured;
  auto lat = std::make_unique<adsb::replay::PipelineLatency>();
  const adsb::replay::ReplayResult r =
      pipeline ? adsb::replay::replay_pipeline(rd, *engine, opt, lat.get(), reader_cpu, worker_cpu, capture)
               : adsb::replay::replay_inline(rd, *engine, opt, capture);

  if (!out_path.empty()) {
    std::FILE* f = std::fopen(out_path.c_str(), "wb");
    const bool ok = f && std::fwrite(captured.data(), 1, captured.size(), f) == captured.size();
    if (f && std::fclose(f) != 0) f = nullptr;
    if (!ok || !f) {
      std::fprintf(stderr, "adsb-replay: cannot write %s\n", out_path.c_str());
      return 1;
    }
  }

  std::string book_hex;
  std::size_t book_size = 0;
  if (book_digest) {
    adsb::feed::FeedBook book;
    for (std::size_t off = 0; off + 2 <= captured.size();) {
      std::uint16_t len;
      std::memcpy(&len, captured.data() + off, 2);
      book.apply(std::span<const std::uint8_t>(captured.data() + off + 2, len));
      off += 2u + len;
    }
    book_hex = book.hex_digest();
    book_size = book.size();
  }
  if (hash_only) {
    std::printf("%s\n", r.hash.c_str());
    if (book_digest) std::printf("%s\n", book_hex.c_str());
    return 0;
  }
  const auto& s = r.stats;
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("recording   %s (%s, %llu records%s)\n", path.c_str(), rd.info().source.c_str(),
              u(rd.count()), rd.truncated() ? ", truncated" : "");
  std::printf("build       %s, %s\n", adsb::build_variant(), pipeline ? "pipeline (2 threads)" : "inline");
  std::printf("replayed    %llu records, %.1f s of traffic in %.3f s (%.0f records/s)\n", u(r.records),
              r.recording_seconds, r.wall_seconds,
              r.wall_seconds > 0 ? static_cast<double>(r.records) / r.wall_seconds : 0.0);
  std::printf("frames      bad %llu, duplicates %llu, out-of-order %llu, A/P accepted %llu / rejected %llu\n",
              u(s.bad_frames), u(s.duplicates), u(s.out_of_order), u(s.ap_accepted), u(s.ap_rejected));
  std::printf("positions   global %llu, local %llu, unresolved %llu, rejected %llu, re-anchored %llu\n",
              u(s.cpr_global), u(s.cpr_local), u(s.cpr_unresolved), u(s.cpr_rejected), u(s.cpr_reanchored));
  std::printf("tracks      created %llu, expired %llu, live %zu, table full %llu\n", u(s.tracks_created),
              u(s.tracks_expired), engine->track_count(), u(s.table_full));
  std::printf("output      %llu updates, %llu deletes\n", u(r.updates), u(r.deletes));
  std::printf("output hash %s\n", r.hash.c_str());
  if (pipeline) {
    std::printf("latency (per record):\n");
    if (opt.speed > 0.0) print_hist("release lag", lat->release_lag_ns);
    print_hist("queue", lat->queue_ns);
    print_hist("decode+state", lat->process_ns);
    print_hist("total", lat->total_ns);
  }
  if (book_digest) {
    std::printf("book        %zu tracks\n", book_size);
    std::printf("book digest %s\n", book_hex.c_str());
  }
  return 0;
}
