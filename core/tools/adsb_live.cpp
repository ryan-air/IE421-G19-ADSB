// adsb-live: run the pipeline on a live Beast feed (readsb / dump1090).
//
//   adsb-live [--host 127.0.0.1] [--port 30005] [--record FILE.adsbrec]
//             [--duration SECONDS] [--receiver LAT,LON] [--stats-every SECONDS]
//             [--busy-poll] [--cpus INGEST,STATE[,RECORDER]] [--line N] [--quiet]
//
// Threads:
//   ingest    BeastTcpSource: recv -> parse -> Record (rx_ns = receive time)
//               -> StageQueue -> state thread
//               -> recorder queue (if --record; dropped and counted if full,
//                  so a slow disk never stalls ingest)
//   state     decode + track engine; per-frame latency histogram from receive
//             to track state updated; expires idle tracks on event time
//   recorder  writes the .adsbrec file
//
// Stop with Ctrl-C (or --duration): queues drain, the recording gets its
// footer, and final counters are printed.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "adsb/common/build_config.hpp"
#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/common/spsc_ring.hpp"
#include "adsb/common/thread.hpp"
#include "adsb/ingest/beast_tcp.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/state/track_engine.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-live [options]\n"
               "  --host HOST           Beast source host (default 127.0.0.1)\n"
               "  --port PORT           Beast output port (default 30005)\n"
               "  --record FILE         also record every frame to an .adsbrec file\n"
               "  --duration SECONDS    stop after this long (default: run until Ctrl-C)\n"
               "  --receiver LAT,LON    receiver position (surface positions, recording header)\n"
               "  --stats-every SECONDS print counters this often (default 10, 0 = only at the end)\n"
               "  --busy-poll           spin on the socket instead of poll() (lowest latency, uses a core)\n"
               "  --cpus I,S[,R]        pin ingest, state, recorder threads (Linux only)\n"
               "  --line N              receiver line id to stamp on frames (default 0)\n"
               "  --quiet               no connection status messages\n");
  std::exit(code);
}

struct Item {
  adsb::record::Record rec;
  std::uint64_t t_ingest;  // ticks when the ingest thread handed it over
};

// Counters the state thread publishes for the periodic report.
struct Shared {
  std::atomic<std::uint64_t> frames{0}, tracks{0}, updates{0}, deletes{0}, bad{0}, positions{0};
  std::atomic<std::uint64_t> recorded{0}, record_dropped{0};
};

struct CountingSink {
  Shared* shared;
  std::uint64_t updates = 0, deletes = 0, positions = 0;
  void on_update(const adsb::protocol::TrackUpdate& u) {
    ++updates;
    if (u.update_mask & adsb::protocol::update_mask::kPosition) ++positions;
  }
  void on_delete(const adsb::protocol::TrackDelete&) { ++deletes; }
};

void print_hist(const char* name, const adsb::LatencyHistogram& h) {
  std::printf("  %-22s p50 %8llu  p99 %8llu  p99.9 %8llu  max %10llu ns  (n=%llu)\n", name,
              static_cast<unsigned long long>(h.percentile(50)),
              static_cast<unsigned long long>(h.percentile(99)),
              static_cast<unsigned long long>(h.percentile(99.9)),
              static_cast<unsigned long long>(h.max()),
              static_cast<unsigned long long>(h.count()));
}

}  // namespace

int main(int argc, char** argv) {
  adsb::ingest::BeastTcpConfig src_cfg;
  adsb::state::EngineConfig eng_cfg;
  std::string record_path;
  double duration_s = 0, stats_every_s = 10;
  bool quiet = false;
  int cpu_ingest = -1, cpu_state = -1, cpu_recorder = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--host") src_cfg.host = val();
    else if (a == "--port") {
      const long p = std::strtol(val().c_str(), nullptr, 10);
      if (p <= 0 || p > 65535) usage(1);
      src_cfg.port = static_cast<std::uint16_t>(p);
    } else if (a == "--record") record_path = val();
    else if (a == "--duration") duration_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--stats-every") stats_every_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--busy-poll") src_cfg.busy_poll = true;
    else if (a == "--quiet") quiet = true;
    else if (a == "--line") src_cfg.line = static_cast<std::uint8_t>(std::strtol(val().c_str(), nullptr, 10) & 0xF);
    else if (a == "--receiver") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%lf,%lf", &eng_cfg.receiver_lat, &eng_cfg.receiver_lon) != 2) usage(1);
      eng_cfg.has_receiver = true;
    } else if (a == "--cpus") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%d,%d,%d", &cpu_ingest, &cpu_state, &cpu_recorder) < 2) usage(1);
    } else {
      std::fprintf(stderr, "adsb-live: unknown option %s\n", a.c_str());
      usage(1);
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  // Recorder (optional).
  adsb::record::RecordingWriter writer;
  if (!record_path.empty()) {
    adsb::record::RecordingInfo info;
    info.source = "readsb-beast";
    info.start_wall_ns = adsb::wall_ns();
    info.has_receiver_position = eng_cfg.has_receiver;
    info.receiver_lat = eng_cfg.receiver_lat;
    info.receiver_lon = eng_cfg.receiver_lon;
    std::string err;
    if (!writer.open(record_path, info, &err)) {
      std::fprintf(stderr, "adsb-live: %s\n", err.c_str());
      return 1;
    }
  }

  using StateQueue = adsb::StageQueue<Item, 65536>;
  using RecQueue = adsb::SpscRing<adsb::record::Record, 65536>;
  auto state_q = std::make_unique<StateQueue>();
  auto rec_q = record_path.empty() ? nullptr : std::make_unique<RecQueue>();
  auto engine = std::make_unique<adsb::DefaultTrackEngine>(eng_cfg);
  auto recv_to_state = std::make_unique<adsb::LatencyHistogram>();
  auto queue_wait = std::make_unique<adsb::LatencyHistogram>();
  const adsb::TickConverter conv = adsb::TickConverter::calibrate();
  Shared shared;
  std::atomic<bool> ingest_done{false}, state_done{false};

  adsb::ingest::BeastTcpSource source(src_cfg);
  const std::uint64_t t_start = adsb::mono_ns();

  std::thread ingest([&] {
    adsb::set_current_thread_name("adsb-ingest");
    if (cpu_ingest >= 0) adsb::pin_current_thread(cpu_ingest);
    Item it{};
    source.run(
        g_stop,
        [&](const adsb::record::Record& r) {
          if (rec_q) {
            if (rec_q->try_push(r)) {
              shared.recorded.fetch_add(1, std::memory_order_relaxed);
            } else {
              shared.record_dropped.fetch_add(1, std::memory_order_relaxed);
            }
          }
          it.rec = r;
          it.t_ingest = adsb::ticks();
          while (!state_q->try_push(it)) {
            if (g_stop.load(std::memory_order_relaxed)) return;
            adsb::cpu_relax();
          }
        },
        [&](const char* status) {
          if (!quiet) std::fprintf(stderr, "adsb-live: %s:%u %s\n", src_cfg.host.c_str(), src_cfg.port, status);
        });
    ingest_done.store(true, std::memory_order_release);
  });

  std::thread state([&] {
    adsb::set_current_thread_name("adsb-state");
    if (cpu_state >= 0) adsb::pin_current_thread(cpu_state);
    CountingSink sink{&shared};
    auto publish = [&](std::uint64_t bad) {
      shared.tracks.store(engine->track_count(), std::memory_order_relaxed);
      shared.updates.store(sink.updates, std::memory_order_relaxed);
      shared.deletes.store(sink.deletes, std::memory_order_relaxed);
      shared.positions.store(sink.positions, std::memory_order_relaxed);
      shared.bad.store(bad, std::memory_order_relaxed);
    };
    Item it{};
    std::uint64_t idle_spins = 0;
    for (;;) {
      if (state_q->try_pop(it)) {
        idle_spins = 0;
        const std::uint64_t t_pop = adsb::ticks();
        const auto& r = it.rec;
        if (r.type == '2' || r.type == '3') {  // Mode S only; Mode A/C carries no address
          adsb::state::FrameMeta m;
          m.rx_ns = r.rx_ns;
          m.radio_ts = r.radio_ts;
          m.line = r.line;
          engine->on_frame(r.payload, r.len, m, sink);
        }
        queue_wait->record(conv.to_ns(t_pop - it.t_ingest));
        // rx_ns was taken right after recv() (mono_ns domain): this covers
        // parse + handoff + decode + track state, i.e. receive -> state done.
        const std::uint64_t done_ns = adsb::mono_ns();
        recv_to_state->record(done_ns > r.rx_ns ? done_ns - r.rx_ns : 0);
        shared.frames.fetch_add(1, std::memory_order_relaxed);
        publish(engine->stats().bad_frames);
      } else {
        if (ingest_done.load(std::memory_order_acquire) && state_q->empty_approx()) break;
        // Idle: let tracks expire on (event) time even when no frames arrive.
        if (++idle_spins % 4096 == 0) {
          engine->advance(adsb::mono_ns(), sink);
          publish(engine->stats().bad_frames);
        }
        adsb::cpu_relax();
      }
    }
    state_done.store(true, std::memory_order_release);
  });

  std::thread recorder;
  if (rec_q) {
    recorder = std::thread([&] {
      adsb::set_current_thread_name("adsb-recorder");
      if (cpu_recorder >= 0) adsb::pin_current_thread(cpu_recorder);
      adsb::record::Record r;
      for (;;) {
        if (rec_q->try_pop(r)) {
          (void)writer.append(r);
        } else if (ingest_done.load(std::memory_order_acquire) && rec_q->empty_approx()) {
          break;
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      }
    });
  }

  // Main thread: periodic report and the --duration timer.
  std::uint64_t next_report = t_start + static_cast<std::uint64_t>(stats_every_s * 1e9);
  std::uint64_t last_frames = 0, last_t = t_start;
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t now = adsb::mono_ns();
    if (duration_s > 0 && now - t_start >= static_cast<std::uint64_t>(duration_s * 1e9)) g_stop.store(true);
    if (stats_every_s > 0 && now >= next_report) {
      const std::uint64_t f = shared.frames.load();
      std::printf("[%6.1f s] %llu frames (%.0f/s), %llu tracks, %llu position updates, %llu bad frames%s\n",
                  static_cast<double>(now - t_start) * 1e-9, static_cast<unsigned long long>(f),
                  static_cast<double>(f - last_frames) / (static_cast<double>(now - last_t) * 1e-9),
                  static_cast<unsigned long long>(shared.tracks.load()),
                  static_cast<unsigned long long>(shared.positions.load()),
                  static_cast<unsigned long long>(shared.bad.load()),
                  rec_q ? (", recorded " + std::to_string(shared.recorded.load())).c_str() : "");
      std::fflush(stdout);
      last_frames = f;
      last_t = now;
      next_report = now + static_cast<std::uint64_t>(stats_every_s * 1e9);
    }
  }

  ingest.join();
  state.join();
  if (recorder.joinable()) recorder.join();
  const bool rec_ok = record_path.empty() || writer.close();

  // Final report (all worker threads have finished; reading their state is safe).
  const auto s = source.stats();
  const auto& es = engine->stats();
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("source      %s:%u, %llu connects, %llu disconnects, %llu failed attempts\n", src_cfg.host.c_str(),
              src_cfg.port, u(s.connects), u(s.disconnects), u(s.connect_failures));
  std::printf("stream      %llu bytes, %llu frames, %llu truncated, %llu bad type, %llu bytes skipped\n",
              u(s.bytes), u(s.frames), u(s.parser.truncated), u(s.parser.bad_type), u(s.parser.skipped_bytes));
  std::printf("engine      %llu frames, %llu bad, %llu duplicates, A/P accepted %llu / rejected %llu\n",
              u(es.frames), u(es.bad_frames), u(es.duplicates), u(es.ap_accepted), u(es.ap_rejected));
  std::printf("positions   global %llu, local %llu, unresolved %llu, rejected %llu\n", u(es.cpr_global),
              u(es.cpr_local), u(es.cpr_unresolved), u(es.cpr_rejected));
  std::printf("tracks      created %llu, expired %llu, live %zu\n", u(es.tracks_created), u(es.tracks_expired),
              engine->track_count());
  if (!record_path.empty()) {
    std::printf("recording   %s: %llu records, %llu dropped%s\n", record_path.c_str(), u(writer.count()),
                u(shared.record_dropped.load()), rec_ok ? "" : " (WRITE FAILED)");
  }
  std::printf("build       %s\n", adsb::build_variant());
  std::printf("latency (receive -> track state updated, per frame):\n");
  print_hist("queue wait", *queue_wait);
  print_hist("receive -> state done", *recv_to_state);
  return rec_ok ? 0 : 1;
}
