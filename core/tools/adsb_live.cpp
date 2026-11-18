// adsb-live: run the pipeline on a live Beast feed (readsb / dump1090).
//
//   adsb-live [--host 127.0.0.1] [--port 30005] [--record FILE.adsbrec]
//             [--duration SECONDS] [--receiver LAT,LON] [--stats-every SECONDS]
//             [--busy-poll] [--cpus INGEST,STATE[,RECORDER]] [--line N] [--quiet]
//             [--publish GROUP:PORT [--publish-interface ADDR] [--batch-us N] ...]
//
// Threads:
//   ingest    BeastTcpSource: recv -> parse -> Record (rx_ns = receive time)
//               -> StageQueue -> state thread
//               -> recorder queue (if --record; dropped and counted if full,
//                  so a slow disk never stalls ingest)
//   state     decode + track engine; per-frame latency histogram from receive
//             to track state updated; expires idle tracks on event time.
//             With --publish, the engine's output also goes to the feed
//             publisher (README_UPDATES.md §9), and the histogram then covers
//             receive -> published.
//   recovery  (with --publish) retransmit and snapshot servers
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
#include "adsb/feed/net.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/conflated_feed.hpp"
#include "adsb/feed/recovery.hpp"
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
               "overload (README_UPDATES.md §10.1):\n"
               "  --overload drop|block when decode/state falls behind: drop the frame at the input and\n"
               "                        count it (default), or stop reading the socket until it catches up\n"
               "                        (pressure then goes upstream, into TCP and readsb, uncounted)\n"
               "  --max-queue N         drop once N frames are waiting (bounds the queueing delay;\n"
               "                        default: the queue's capacity, 65536)\n"
               "  --simulate-work-ns N  TESTING ONLY: spin N ns per frame in the state stage, to give the\n"
               "                        pipeline a known saturation rate (about 1e9 / N frames/s)\n"
               "  --quiet               no connection status messages\n"
               "feed (README_UPDATES.md §9):\n"
               "  --publish HOST:PORT   publish tracks on this multicast group (or unicast address)\n"
               "  --publish-interface A multicast egress interface address (default: kernel's choice)\n"
               "  --batch-us N          hold a datagram at most N microseconds while busy (default 100;\n"
               "                        it is sent at once whenever the input queue is empty, so light\n"
               "                        load adds no delay; 0 = one datagram per update)\n"
               "  --retransmit-port P   UDP retransmit server (default 41901)\n"
               "  --snapshot-port P     TCP snapshot server (default 41902)\n"
               "  --session ID          session id, up to 10 characters (default: from the clock)\n"
               "conflated feed (README_UPDATES.md §10.2; latest state per aircraft at a fixed rate):\n"
               "  --conflate GROUP:PORT also publish the conflated feed on this group (e.g. 239.19.0.3:41910)\n"
               "  --conflate-hz N       ticks per second (default 10)\n"
               "  --conflate-retransmit-port P (default 41911)   --conflate-snapshot-port P (default 41912)\n");
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
  // Overload at the input boundary (README_UPDATES.md §10.1): frames dropped
  // because the decode/state stage was behind, and the deepest the input
  // queue got. Written by the ingest thread only.
  std::atomic<std::uint64_t> overload_dropped{0}, queue_high_water{0};
};

using FeedPublisher = adsb::feed::Publisher<adsb::feed::UdpSender>;

struct CountingSink {
  Shared* shared;
  FeedPublisher* pub = nullptr;            // --publish
  adsb::feed::ConflatedFeed* conf = nullptr;  // --conflate
  std::uint64_t updates = 0, deletes = 0, positions = 0;
  void on_update(const adsb::protocol::TrackUpdate& u) {
    ++updates;
    if (u.update_mask & adsb::protocol::update_mask::kPosition) ++positions;
    if (pub) pub->on_update(u);
    if (conf) conf->on_update(u);
  }
  void on_delete(const adsb::protocol::TrackDelete& d) {
    ++deletes;
    if (pub) pub->on_delete(d);
    if (conf) conf->on_delete(d);
  }
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
  std::string publish_to, session_id;
  adsb::feed::UdpSender::Options send_opt;
  adsb::feed::PublisherConfig pub_cfg;
  std::string conflate_to;
  adsb::feed::ConflatedFeedConfig conf_cfg;
  pub_cfg.max_batch_delay_ns = 100'000;  // see --batch-us
  bool overload_block = false;           // --overload block
  std::size_t max_queue = 0;             // --max-queue (0 = the queue's capacity)
  std::uint64_t work_ns = 0;             // --simulate-work-ns (overload tests only)
  adsb::feed::RecoveryServerConfig rec_cfg;
  rec_cfg.retransmit_port = 41901;
  rec_cfg.snapshot_port = 41902;
  auto parse_port = [](const std::string& v) -> std::uint16_t {
    const long p = std::strtol(v.c_str(), nullptr, 10);
    if (p <= 0 || p > 65535) usage(1);
    return static_cast<std::uint16_t>(p);
  };

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
    else if (a == "--conflate") conflate_to = val();
    else if (a == "--conflate-hz") conf_cfg.hz = std::strtod(val().c_str(), nullptr);
    else if (a == "--conflate-retransmit-port") conf_cfg.retransmit_port = parse_port(val());
    else if (a == "--conflate-snapshot-port") conf_cfg.snapshot_port = parse_port(val());
    else if (a == "--overload") {
      const std::string v = val();
      if (v == "drop") overload_block = false;
      else if (v == "block") overload_block = true;
      else usage(2);
    } else if (a == "--max-queue") max_queue = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--simulate-work-ns") work_ns = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--line") src_cfg.line = static_cast<std::uint8_t>(std::strtol(val().c_str(), nullptr, 10) & 0xF);
    else if (a == "--receiver") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%lf,%lf", &eng_cfg.receiver_lat, &eng_cfg.receiver_lon) != 2) usage(1);
      eng_cfg.has_receiver = true;
    } else if (a == "--cpus") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%d,%d,%d", &cpu_ingest, &cpu_state, &cpu_recorder) < 2) usage(1);
    } else if (a == "--publish") publish_to = val();
    else if (a == "--publish-interface") send_opt.interface_host = val();
    else if (a == "--batch-us") pub_cfg.max_batch_delay_ns = std::strtoull(val().c_str(), nullptr, 10) * 1000;
    else if (a == "--retransmit-port") rec_cfg.retransmit_port = parse_port(val());
    else if (a == "--snapshot-port") rec_cfg.snapshot_port = parse_port(val());
    else if (a == "--session") {
      session_id = val();
      if (session_id.empty() || session_id.size() > 10) usage(1);
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

  // Feed publisher and recovery servers (optional).
  std::unique_ptr<adsb::feed::UdpSender> udp;
  std::unique_ptr<adsb::feed::MessageLog> feed_log;
  std::unique_ptr<adsb::feed::RecoveryServer> recovery;
  std::unique_ptr<FeedPublisher> pub;
  std::atomic<bool> rec_stop{false};
  std::thread rec_thread;
  if (!publish_to.empty()) {
    adsb::feed::Endpoint dest;
    if (!adsb::feed::parse_endpoint(publish_to, dest)) usage(1);
    if (session_id.empty()) {
      const adsb::protocol::Session s = adsb::feed::unique_session('L');
      session_id.assign(s.begin(), s.end());
    }
    pub_cfg.session = adsb::protocol::make_session(session_id.c_str());
    rec_cfg.recovery.session = pub_cfg.session;
    std::string err;
    udp = std::make_unique<adsb::feed::UdpSender>();
    if (!udp->open(dest, send_opt, &err)) {
      std::fprintf(stderr, "adsb-live: publish %s: %s\n", publish_to.c_str(), err.c_str());
      return 1;
    }
    feed_log = adsb::feed::make_message_log();
    recovery = std::make_unique<adsb::feed::RecoveryServer>(rec_cfg, *feed_log);
    if (!recovery->open(&err)) {
      std::fprintf(stderr, "adsb-live: recovery servers: %s\n", err.c_str());
      return 1;
    }
    pub = std::make_unique<FeedPublisher>(pub_cfg, *udp, feed_log.get());
    rec_thread = std::thread([&] {
      adsb::set_current_thread_name("adsb-recovery");
      recovery->run(rec_stop);
    });
    if (!quiet) {
      std::fprintf(stderr, "adsb-live: publishing session %s on %s (retransmit :%u, snapshot :%u)\n",
                   session_id.c_str(), publish_to.c_str(), recovery->retransmit_port(), recovery->snapshot_port());
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
    std::size_t high_water = 0;
    const std::size_t queue_limit =
        max_queue == 0 || max_queue > StateQueue::kCapacity ? StateQueue::kCapacity : max_queue;
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
          // Input boundary. Everything after it is lossless; this is the one
          // place a frame may be lost, and it is counted (README_UPDATES.md
          // §10.1). With --overload block the ingest stops reading instead.
          const std::size_t depth = state_q->size_approx();
          if (depth > high_water) {
            high_water = depth;
            shared.queue_high_water.store(depth, std::memory_order_relaxed);
          }
          if (!overload_block && depth >= queue_limit) {
            shared.overload_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          while (!state_q->try_push(it)) {
            if (g_stop.load(std::memory_order_relaxed)) return;
            if (!overload_block) {  // full despite the check (limit == capacity)
              shared.overload_dropped.fetch_add(1, std::memory_order_relaxed);
              return;
            }
            adsb::cpu_relax();
          }
        },
        [&](const char* status) {
          if (!quiet) std::fprintf(stderr, "adsb-live: %s:%u %s\n", src_cfg.host.c_str(), src_cfg.port, status);
        });
    ingest_done.store(true, std::memory_order_release);
  });

  adsb::feed::ConflatedFeed conflated;
  bool conf_failed = false;
  if (!conflate_to.empty()) {
    if (!adsb::feed::parse_endpoint(conflate_to, conf_cfg.group)) usage(1);
    conf_cfg.send = send_opt;
    conf_cfg.bind_host = rec_cfg.bind_host;
    std::string err;
    if (!conflated.open(conf_cfg, &err)) {
      std::fprintf(stderr, "adsb-live: conflated feed %s: %s\n", conflate_to.c_str(), err.c_str());
      conf_failed = true;
      g_stop = true;
    } else if (!quiet) {
      std::fprintf(stderr, "adsb-live: conflated feed session %s on %s at %.3g Hz (retransmit :%u, snapshot :%u)\n",
                   conflated.session_text().c_str(), conflate_to.c_str(), conf_cfg.hz,
                   conflated.recovery().retransmit_port(), conflated.recovery().snapshot_port());
    }
  }
  adsb::feed::ConflatedFeed* conf = conflate_to.empty() || g_stop.load() ? nullptr : &conflated;

  std::thread state([&] {
    adsb::set_current_thread_name("adsb-state");
    if (cpu_state >= 0) adsb::pin_current_thread(cpu_state);
    CountingSink sink{&shared, pub.get(), conf};
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
        if (conf) conf->poll(r.rx_ns);  // rx_ns is mono_ns() at recv: ticks keep time under load
        if (work_ns) {  // --simulate-work-ns: a slower stage, for overload tests
          const std::uint64_t until = adsb::mono_ns() + work_ns;
          while (adsb::mono_ns() < until) adsb::cpu_relax();
        }
        queue_wait->record(conv.to_ns(adsb::tick_delta(t_pop, it.t_ingest)));
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
        // Batches due and heartbeats while the feed is quiet.
        // Batch only while busy: the moment the input queue runs dry, send
        // what is pending (no added delay at light load).
        if (pub && idle_spins == 1) pub->flush();
        if (pub && idle_spins % 64 == 0) pub->poll();
        if (conf && idle_spins % 64 == 0) conf->poll(adsb::mono_ns());
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
      std::printf("[%6.1f s] %llu frames (%.0f/s), %llu tracks, %llu position updates, %llu bad frames, "
                  "%llu dropped (overload)%s\n",
                  static_cast<double>(now - t_start) * 1e-9, static_cast<unsigned long long>(f),
                  static_cast<double>(f - last_frames) / (static_cast<double>(now - last_t) * 1e-9),
                  static_cast<unsigned long long>(shared.tracks.load()),
                  static_cast<unsigned long long>(shared.positions.load()),
                  static_cast<unsigned long long>(shared.bad.load()),
                  static_cast<unsigned long long>(shared.overload_dropped.load()),
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
  if (pub || conf) {
    // End the session(s), then keep answering recovery requests briefly so
    // subscribers can fill the tail.
    if (pub) pub->end_session();
    if (conf) conf->end_session();
    const std::uint64_t until = adsb::mono_ns() + 1'000'000'000ULL;
    while (adsb::mono_ns() < until) {
      if (pub) pub->poll();
      if (conf) conf->poll_ended();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (pub) {
      rec_stop = true;
      rec_thread.join();
    }
    if (conf) conf->stop();
  }
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
  if (pub) {
    const auto& ps = pub->stats();
    const auto& rc = recovery->counters();
    std::printf("feed        session %s: %llu messages in %llu datagrams, %llu heartbeats, %llu send failures; "
                "%llu retransmit requests, %llu snapshots\n",
                session_id.c_str(), u(ps.messages), u(ps.packets), u(ps.heartbeats), u(ps.send_failures),
                u(rc.requests.load()), u(rc.snapshots.load()));
    std::printf("state digest %s\n", recovery->state().book().hex_state_digest().c_str());
    std::printf("book digest %s\n", recovery->state().book().hex_digest().c_str());
  }
  if (conf) {
    const auto& cs = conf->conflator_stats();
    std::printf("conflated   session %s: %llu updates in -> %llu out, %llu deletes out, %llu suppressed, "
                "%llu passed through, %llu ticks, %llu datagrams; %llu retransmit requests, %llu snapshots\n",
                conf->session_text().c_str(), u(cs.updates_in), u(cs.updates_out), u(cs.deletes_out),
                u(cs.suppressed), u(cs.passthrough), u(cs.ticks), u(conf->publisher_stats().packets),
                u(conf->recovery().counters().requests.load()), u(conf->recovery().counters().snapshots.load()));
    std::printf("conflated state digest %s\n", conf->recovery().state().book().hex_state_digest().c_str());
  }
  std::printf("overload    policy %s, %llu frames dropped at the input boundary (%.4f%%), "
              "queue high water %llu of %zu\n",
              overload_block ? "block" : "drop", u(shared.overload_dropped.load()),
              s.frames ? 100.0 * static_cast<double>(shared.overload_dropped.load()) / static_cast<double>(s.frames) : 0.0,
              u(shared.queue_high_water.load()),
              max_queue == 0 || max_queue > StateQueue::kCapacity ? StateQueue::kCapacity : max_queue);
  std::printf("build       %s\n", adsb::build_variant());
  std::printf("latency (receive -> track state updated%s, per frame):\n", pub ? " and published" : "");
  print_hist("queue wait", *queue_wait);
  print_hist("receive -> state done", *recv_to_state);
  return rec_ok && !conf_failed ? 0 : 1;
}
