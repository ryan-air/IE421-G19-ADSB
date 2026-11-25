// adsb-live: run the pipeline on a live Beast feed (readsb / dump1090), or
// on raw IQ samples through our own demodulator (--iq), from a file, stdin
// or a live RTL-SDR (--iq rtlsdr[:INDEX|:SERIAL], librtlsdr).
//
//   adsb-live [--host 127.0.0.1] [--port 30005] [--record FILE.adsbrec]
//             [--duration SECONDS] [--receiver LAT,LON] [--stats-every SECONDS]
//             [--busy-poll] [--cpus INGEST,STATE[,RECORDER[,INGEST_B]]] [--line N] [--quiet]
//             [--b-host HOST --b-port PORT | --iq-b PATH] [--arb-window-us N]
//             [--publish GROUP:PORT [--publish-interface ADDR] [--batch-us N] ...]
//
// Threads:
//   ingest    BeastTcpSource: recv -> parse -> Record (rx_ns = receive time),
//             or with --iq the IqSource consumer: IQ blocks -> demodulator ->
//             Record (rx_ns = host time the frame's last sample arrived),
//             fed by an IQ reader thread through a block ring that drops and
//             counts whole blocks when the demodulator falls behind
//               -> StageQueue -> state thread
//               -> recorder queue (if --record; dropped and counted if full,
//                  so a slow disk never stalls ingest)
//   ingest-b  (A/B, README_UPDATES.md §6) the same for line B, into its own
//             queue. The state thread merges both queues by receive time and
//             the arbiter forwards each frame's first copy only.
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

#include <algorithm>
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
#include "adsb/feed/shm_ring.hpp"
#include "adsb/feed/tx.hpp"
#include "adsb/ingest/arbiter.hpp"
#include "adsb/ingest/beast_tcp.hpp"
#include "adsb/ingest/iq_source.hpp"
#include "adsb/ingest/rtl_device.hpp"
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
               "  --cpus I,S[,R[,B]]    pin ingest, state, recorder, line-B ingest threads (Linux only)\n"
               "  --line N              receiver line id to stamp on frames (default 0)\n"
               "  --fix                 repair one wrong bit in DF17/18 frames (readsb's output is already repaired)\n"
               "A/B lines (two receivers at one site, README_UPDATES.md §6):\n"
               "  --b-host HOST         second Beast source, line B (line A is --host/--port; default 127.0.0.1)\n"
               "  --b-port PORT         line B's Beast port (giving either option turns A/B on)\n"
               "  --iq-b PATH           or: a second IQ input as line B (with --iq; same format options)\n"
               "  --arb-window-us N     arbitration window: copies this close are the same frame (default:\n"
               "                        60000 for Beast lines, which readsb sends in batches of up to 50 ms;\n"
               "                        for IQ lines twice the block duration, at least 5000)\n"
               "                        Lines are 0 (A) and 1 (B). Each frame's first copy is forwarded, later\n"
               "                        copies are dropped and counted; --record keeps every copy that\n"
               "                        passed the input boundary (overload drops are counted per line), so\n"
               "                        adsb-replay --arb-window-us W (W as printed) repeats the run's\n"
               "                        decisions exactly.\n"
               "IQ input (our demodulator instead of readsb; README_UPDATES.md §5):\n"
               "  --iq PATH             IQ samples: NAME.sigmf-meta / .sigmf-data (format and rate from the\n"
               "                        metadata), a raw file, or - for stdin; adsb-live stops at its end\n"
               "  --iq-format F         cu8 | cs8 | cs16 | cf32 (raw input)\n"
               "  --iq-rate HZ          sample rate (raw input; default 2.4e6)\n"
               "  --iq-block N          samples per block, like an SDR's USB transfer (default 16384)\n"
               "  --iq-buffers N        blocks the IQ ring holds (default 16); when full, blocks are dropped\n"
               "                        and counted\n"
               "  --iq-speed X          pace the input at X times the sample rate (default 1 = like a receiver;\n"
               "                        0 = as fast as the demodulator goes, waiting instead of dropping, frames\n"
               "                        timed on the sample clock; at most 200)\n"
               "  --iq-drop             with --iq-speed 0: drop when the ring is full, and time frames by arrival\n"
               "                        (a live receiver on stdin, e.g. rtl_sdr -f 1090e6 -s 2.4e6 - | adsb-live\n"
               "                        --iq - --iq-format cu8 --iq-speed 0 --iq-drop)\n"
               "  --iq-fix              correct one wrong bit in DF11/17/18 frames\n"
               "  --iq-first-arrival US echoes: a frame whose direct path failed up to US earlier takes its\n"
               "                        time (default 40; 0 = off)\n"
               "  --iq-duplicate-window US  drop copies of a frame within US (long echoes; default 200, 0 = off)\n"
               "live RTL-SDR (--iq rtlsdr, rtlsdr:INDEX or rtlsdr:SERIAL; needs a build with librtlsdr):\n"
               "                        reads the dongle with librtlsdr's read_async; each USB transfer of\n"
               "                        --iq-block samples (x2 bytes, a multiple of 512) goes into the next free\n"
               "                        --iq-buffers block, or is dropped and counted; --iq-rate sets the rate\n"
               "  --rtl-gain DB|max|auto tuner gain: the nearest supported step, the highest one (default,\n"
               "                        like readsb and dump1090-fa), or the tuner's own AGC\n"
               "  --rtl-freq HZ         centre frequency (default 1090e6)\n"
               "  --rtl-ppm N           crystal correction in ppm (default 0)\n"
               "  --rtl-agc             the RTL2832's digital AGC (readsb --enable-agc)\n"
               "  --rtl-bias-tee        power an LNA through the antenna cable\n"
               "  --rtl-usb-buffers N   librtlsdr transfer buffers (default 0 = its default, 15)\n"
               "  --rtl-stall-s S       no transfer for this long = a dead device, exit 1 (default 3)\n"
               "  TESTING ONLY: --iq fake:FILE plays a cu8 file (or cu8 SigMF) through the same device path,\n"
               "                        paced at --iq-speed; --fake-error-after B, --fake-unplug-after B and\n"
               "                        --fake-stall-after B make it fail, vanish or stall after B bytes\n"
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
               ADSB_TX_OPTIONS_HELP
               "  --batch-us N          hold a datagram at most N microseconds while busy (default 100;\n"
               "                        it is sent at once whenever the input queue is empty, so light\n"
               "                        load adds no delay; 0 = one datagram per update)\n"
               "  --retransmit-port P   UDP retransmit server (default 41901)\n"
               "  --snapshot-port P     TCP snapshot server (default 41902)\n"
               "  --session ID          session id, up to 10 characters (default: from the clock)\n"
               "  --shm NAME            also publish on a shared-memory ring for subscribers on this host\n"
               "                        (e.g. /adsb-feed; read it with adsb-shm-cli). Works without --publish.\n"
               "  --shm-slots N         ring size in messages, a power of two (default 65536)\n"
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
  std::atomic<std::uint64_t> recorded{0}, record_dropped{0}, record_rejected{0};
  // Overload at the input boundary (README_UPDATES.md §10.1): frames dropped
  // because the decode/state stage was behind, and the deepest the input
  // queue got. Written by the ingest thread only.
  std::atomic<std::uint64_t> overload_dropped{0}, queue_high_water{0};
  // Per line (A/B): the same two, line A in [0].
  std::atomic<std::uint64_t> line_dropped[2]{}, line_high_water[2]{};
  // A/B: copies the arbiter dropped, and receive times raised by the merge.
  std::atomic<std::uint64_t> arb_dropped{0}, merge_clamped{0};
};

// UDP multicast and/or the same-host shared-memory ring (either may be absent).
// The ring first: a ring write costs tens of ns, a UDP sendto() microseconds.
using FeedOut = adsb::feed::DualOut<adsb::feed::ShmOut, adsb::feed::FeedTx>;
using FeedPublisher = adsb::feed::Publisher<FeedOut>;

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
  adsb::ingest::IqSourceConfig iq_cfg;
  std::string iq_path, iq_format;
  bool iq_rate_given = false, iq_speed_given = false;
  adsb::ingest::FakeRtlOptions fake_opt;
  adsb::state::EngineConfig eng_cfg;
  std::string record_path;
  double duration_s = 0, stats_every_s = 10;
  bool quiet = false;
  int cpu_ingest = -1, cpu_state = -1, cpu_recorder = -1, cpu_ingest_b = -1;
  adsb::ingest::BeastTcpConfig b_cfg;   // line B (A/B)
  bool b_beast = false, line_given = false, window_given = false;
  std::string iq_b_path;
  adsb::ingest::ArbiterConfig arb_cfg;
  std::string publish_to, session_id, shm_name;
  std::uint64_t shm_slots = adsb::feed::kShmDefaultSlots;
  adsb::feed::UdpSender::Options send_opt;
  adsb::feed::Transport transport = adsb::feed::Transport::kKernel;
  adsb::feed::TxOptions tx_opt;
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
    else if (a == "--shm") shm_name = val();
    else if (a == "--shm-slots") shm_slots = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--simulate-work-ns") work_ns = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--iq") iq_path = val();
    else if (a == "--iq-format") iq_format = val();
    else if (a == "--iq-rate") { iq_cfg.sample_rate_hz = std::strtod(val().c_str(), nullptr); iq_rate_given = true; }
    else if (a == "--iq-block") iq_cfg.block_samples = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--iq-buffers") iq_cfg.buffers = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--iq-speed") { iq_cfg.speed = std::strtod(val().c_str(), nullptr); iq_speed_given = true; }
    else if (a == "--rtl-gain") {
      if (!adsb::ingest::parse_rtl_gain(val(), iq_cfg.rtl.gain, iq_cfg.rtl.gain_db)) usage(1);
    } else if (a == "--rtl-freq") iq_cfg.rtl.freq_hz = std::strtod(val().c_str(), nullptr);
    else if (a == "--rtl-ppm") iq_cfg.rtl.ppm = static_cast<int>(std::strtol(val().c_str(), nullptr, 10));
    else if (a == "--rtl-agc") iq_cfg.rtl.agc = true;
    else if (a == "--rtl-bias-tee") iq_cfg.rtl.bias_tee = true;
    else if (a == "--rtl-usb-buffers") {
      const long n = std::strtol(val().c_str(), nullptr, 10);
      if (n < 0 || n > 1024) usage(1);
      iq_cfg.rtl.usb_buffers = static_cast<std::uint32_t>(n);
    } else if (a == "--rtl-stall-s") {
      const double v = std::strtod(val().c_str(), nullptr);
      if (!(v >= 0.001 && v <= 3600)) usage(1);
      iq_cfg.rtl.stall_timeout_ns = static_cast<std::uint64_t>(v * 1e9);
    }
    else if (a == "--fake-error-after") fake_opt.error_after_bytes = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--fake-unplug-after") fake_opt.unplug_after_bytes = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--fake-stall-after") fake_opt.stall_after_bytes = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--iq-fix") iq_cfg.demod.fix_single_bit = true;
    else if (a == "--iq-first-arrival") iq_cfg.demod.first_arrival_us = std::strtod(val().c_str(), nullptr);
    else if (a == "--iq-duplicate-window") iq_cfg.demod.duplicate_window_us = std::strtod(val().c_str(), nullptr);
    else if (a == "--iq-drop") iq_cfg.drop_when_full = true;
    else if (a == "--fix") eng_cfg.fix_single_bit = true;
    else if (a == "--line") {
      src_cfg.line = static_cast<std::uint8_t>(std::strtol(val().c_str(), nullptr, 10) & 0xF);
      line_given = true;
    } else if (a == "--b-host") { b_cfg.host = val(); b_beast = true; }
    else if (a == "--b-port") { b_cfg.port = parse_port(val()); b_beast = true; }
    else if (a == "--iq-b") iq_b_path = val();
    else if (a == "--arb-window-us") {
      const long long w = std::strtoll(val().c_str(), nullptr, 10);
      if (w < 1 || w > 1'000'000) usage(1);
      arb_cfg.window_ns = static_cast<std::uint64_t>(w) * 1000;
      window_given = true;
    }
    else if (a == "--receiver") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%lf,%lf", &eng_cfg.receiver_lat, &eng_cfg.receiver_lon) != 2) usage(1);
      eng_cfg.has_receiver = true;
    } else if (a == "--cpus") {
      const std::string v = val();
      if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &cpu_ingest, &cpu_state, &cpu_recorder, &cpu_ingest_b) < 2) usage(1);
    } else if (a == "--publish") publish_to = val();
    else if (a == "--publish-interface") send_opt.interface_host = val();
    else if (adsb::feed::is_tx_flag(a)) { if (!adsb::feed::apply_tx_flag(a, val(), transport, tx_opt)) usage(1); }
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

  // A/B: two lines of the same kind (two readsb instances, or two IQ inputs).
  const bool ab = b_beast || !iq_b_path.empty();
  if (ab) {
    if (b_beast && !iq_b_path.empty()) {
      std::fprintf(stderr, "adsb-live: line B is either a Beast source (--b-host/--b-port) or IQ (--iq-b)\n");
      return 1;
    }
    if (b_beast == !iq_path.empty()) {
      std::fprintf(stderr, "adsb-live: both lines must be the same kind: --b-host/--b-port with a Beast line A, "
                           "--iq-b with --iq\n");
      return 1;
    }
    if (line_given) {
      std::fprintf(stderr, "adsb-live: with A/B the lines are 0 (A) and 1 (B); --line does not apply\n");
      return 1;
    }
    if (b_beast && b_cfg.host == src_cfg.host && b_cfg.port == src_cfg.port) {
      std::fprintf(stderr, "adsb-live: line B is the same source as line A (%s:%u); give --b-port\n",
                   b_cfg.host.c_str(), b_cfg.port);
      return 1;
    }
    src_cfg.line = 0;
    b_cfg.line = 1;
    b_cfg.busy_poll = src_cfg.busy_poll;
  }

  // IQ input (one per line).
  const bool iq_mode = !iq_path.empty();
  auto open_iq = [&](const std::string& path, std::uint8_t line,
                     adsb::ingest::IqSourceConfig& cfg) -> std::unique_ptr<adsb::ingest::IqSource> {
    std::string err;
    cfg.path = path;
    // A live RTL-SDR (or the test device that stands in for one).
    const bool fake = path.starts_with("fake:");
    if (adsb::ingest::parse_rtlsdr_path(path) || fake) {
      if (!iq_format.empty() && iq_format != "cu8") {
        std::fprintf(stderr, "adsb-live: an RTL-SDR delivers cu8 (--iq-format does not apply)\n");
        return nullptr;
      }
      if (cfg.drop_when_full || (iq_speed_given && !fake)) {
        std::fprintf(stderr, "adsb-live: --iq-speed / --iq-drop do not apply to a live RTL-SDR (it always drops when full)\n");
        return nullptr;
      }
      cfg.format = adsb::demod::SampleFormat::kCu8;
      cfg.line = line;
      std::unique_ptr<adsb::ingest::FakeRtlDevice> dev;
      if (fake) {
        adsb::ingest::FakeRtlOptions fo = fake_opt;
        fo.path = path.substr(5);
        if (fo.path.ends_with(".sigmf-meta") || fo.path.ends_with(".sigmf-data")) {
          double rate = 0;
          adsb::demod::SampleFormat f{};
          std::string data;
          if (!adsb::ingest::read_sigmf(fo.path, data, f, rate, &err)) {
            std::fprintf(stderr, "adsb-live: %s\n", err.c_str());
            return nullptr;
          }
          if (f != adsb::demod::SampleFormat::kCu8) {
            std::fprintf(stderr, "adsb-live: %s: the fake device plays cu8 only\n", fo.path.c_str());
            return nullptr;
          }
          fo.path = data;
          if (!iq_rate_given) cfg.sample_rate_hz = rate;
        }
        if (!(cfg.speed > 0)) {
          std::fprintf(stderr, "adsb-live: the fake device needs --iq-speed > 0\n");
          return nullptr;
        }
        fo.speed = cfg.speed;
        dev = std::make_unique<adsb::ingest::FakeRtlDevice>(fo);
      } else {
        cfg.speed = 1;
      }
      if (!adsb::ingest::validate(cfg, &err)) {
        std::fprintf(stderr, "adsb-live: %s: %s\n", path.c_str(), err.c_str());
        return nullptr;
      }
      auto src = std::make_unique<adsb::ingest::IqSource>(cfg);
      if (dev) src->use_device(std::move(dev));
      if (!src->open(&err)) {
        std::fprintf(stderr, "adsb-live: %s: %s\n", path.c_str(), err.c_str());
        return nullptr;
      }
      if (!quiet) {
        std::fprintf(stderr, "adsb-live: %s: %s\n", path.c_str(),
                     adsb::ingest::rtl_info_text(src->device_info()).c_str());
      }
      return src;
    }
    const bool sigmf = path.size() > 11 && (path.ends_with(".sigmf-meta") || path.ends_with(".sigmf-data"));
    if (sigmf) {
      double rate = 0;
      if (!adsb::ingest::read_sigmf(path, cfg.path, cfg.format, rate, &err)) {
        std::fprintf(stderr, "adsb-live: %s\n", err.c_str());
        return nullptr;
      }
      if (!iq_rate_given) cfg.sample_rate_hz = rate;
    } else if (iq_format.empty() || !adsb::demod::parse_format(iq_format, cfg.format)) {
      std::fprintf(stderr, "adsb-live: raw --iq input needs --iq-format cu8|cs8|cs16|cf32\n");
      return nullptr;
    }
    if (!iq_format.empty() && sigmf) {
      std::fprintf(stderr, "adsb-live: --iq-format is for raw input; SigMF gives the format\n");
      return nullptr;
    }
    cfg.line = line;
    if (!adsb::ingest::validate(cfg, &err)) {
      std::fprintf(stderr, "adsb-live: %s: %s\n", path.c_str(), err.c_str());
      return nullptr;
    }
    auto src = std::make_unique<adsb::ingest::IqSource>(cfg);
    if (!src->open(&err)) {
      std::fprintf(stderr, "adsb-live: %s: %s\n", path.c_str(), err.c_str());
      return nullptr;
    }
    return src;
  };
  if (ab && iq_mode && iq_cfg.speed <= 0 && !iq_cfg.drop_when_full) {
    std::fprintf(stderr, "adsb-live: A/B with IQ needs paced input (--iq-speed > 0) or a live receiver (--iq-drop):\n"
                         "unpaced, each line runs at its own speed on its own sample clock\n");
    return 1;
  }
  std::unique_ptr<adsb::ingest::IqSource> iq_source, iq_source_b;
  adsb::ingest::IqSourceConfig iq_cfg_b = iq_cfg;
  if (iq_mode) {
    iq_source = open_iq(iq_path, src_cfg.line, iq_cfg);
    if (!iq_source) return 1;
  }
  if (!iq_b_path.empty()) {
    iq_source_b = open_iq(iq_b_path, 1, iq_cfg_b);
    if (!iq_source_b) return 1;
  }
  if (ab && !window_given) {
    // The window must cover how unevenly the lines deliver: readsb flushes its
    // Beast output every 50 ms or 1,280 bytes, and an IQ line hands over a
    // block's frames together (rates from the SigMF metadata, now read).
    // Squitters repeat identical payloads no sooner than ~400 ms.
    if (b_beast) {
      arb_cfg.window_ns = 60'000'000;
    } else {
      auto block_ns = [](const adsb::ingest::IqSourceConfig& c) {
        const double speed = c.speed > 0 ? c.speed : 1.0;
        return static_cast<double>(c.block_samples) / c.sample_rate_hz * 1e9 / speed;
      };
      const double blk = std::max(block_ns(iq_cfg), block_ns(iq_cfg_b));
      arb_cfg.window_ns = std::max<std::uint64_t>(5'000'000, static_cast<std::uint64_t>(2 * blk));
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  // Recorder (optional).
  adsb::record::RecordingWriter writer;
  if (!record_path.empty()) {
    adsb::record::RecordingInfo info;
    info.source = iq_mode ? "adsb-demod" : "readsb-beast";
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
  std::unique_ptr<adsb::feed::FeedTx> udp;
  adsb::feed::ShmRegion shm_region;
  std::unique_ptr<adsb::feed::ShmOut> shm_out;
  std::unique_ptr<FeedOut> feed_out;
  std::unique_ptr<adsb::feed::MessageLog> feed_log;
  std::unique_ptr<adsb::feed::RecoveryServer> recovery;
  std::unique_ptr<FeedPublisher> pub;
  std::atomic<bool> rec_stop{false};
  std::thread rec_thread;
  if (!publish_to.empty() || !shm_name.empty()) {
    adsb::feed::Endpoint dest;
    if (!publish_to.empty() && !adsb::feed::parse_endpoint(publish_to, dest)) usage(1);
    if (session_id.empty()) {
      const adsb::protocol::Session s = adsb::feed::unique_session('L');
      session_id.assign(s.begin(), s.end());
    }
    pub_cfg.session = adsb::protocol::make_session(session_id.c_str());
    rec_cfg.recovery.session = pub_cfg.session;
    std::string err;
    if (!publish_to.empty()) {
      udp = std::make_unique<adsb::feed::FeedTx>();
      if (!udp->open(dest, transport, send_opt, tx_opt, &err)) {
        std::fprintf(stderr, "adsb-live: publish %s: %s\n", publish_to.c_str(), err.c_str());
        return 1;
      }
    }
    if (!shm_name.empty()) {
      if (!shm_region.create(shm_name, shm_slots, pub_cfg.session, pub_cfg.first_seq, &err)) {
        std::fprintf(stderr, "adsb-live: shm %s: %s\n", shm_name.c_str(), err.c_str());
        return 1;
      }
      shm_out = std::make_unique<adsb::feed::ShmOut>(shm_region.view());
    }
    feed_out = std::make_unique<FeedOut>(shm_out.get(), udp.get());
    feed_log = adsb::feed::make_message_log();
    recovery = std::make_unique<adsb::feed::RecoveryServer>(rec_cfg, *feed_log);
    if (!recovery->open(&err)) {
      std::fprintf(stderr, "adsb-live: recovery servers: %s\n", err.c_str());
      return 1;
    }
    pub = std::make_unique<FeedPublisher>(pub_cfg, *feed_out, feed_log.get());
    rec_thread = std::thread([&] {
      adsb::set_current_thread_name("adsb-recovery");
      recovery->run(rec_stop);
    });
    if (!quiet) {
      std::fprintf(stderr, "adsb-live: publishing session %s on %s%s%s (retransmit :%u, snapshot :%u)\n",
                   session_id.c_str(), publish_to.empty() ? "" : publish_to.c_str(),
                   publish_to.empty() || shm_name.empty() ? "" : " and ",
                   shm_name.empty() ? "" : ("shm " + shm_name).c_str(), recovery->retransmit_port(),
                   recovery->snapshot_port());
    }
  }

  using StateQueue = adsb::StageQueue<Item, 65536>;
  using RecQueue = adsb::SpscRing<adsb::record::Record, 65536>;
  auto state_q = std::make_unique<StateQueue>();
  auto state_qb = ab ? std::make_unique<StateQueue>() : nullptr;  // line B
  auto rec_q = record_path.empty() ? nullptr : std::make_unique<RecQueue>();
  auto engine = std::make_unique<adsb::DefaultTrackEngine>(eng_cfg);
  auto arbiter = ab ? std::make_unique<adsb::ingest::Arbiter>(arb_cfg) : nullptr;
  auto recv_to_state = std::make_unique<adsb::LatencyHistogram>();
  auto queue_wait = std::make_unique<adsb::LatencyHistogram>();
  const adsb::TickConverter conv = adsb::TickConverter::calibrate();
  Shared shared;
  std::atomic<bool> ingest_done{false}, state_done{false};
  std::atomic<int> ingest_running{ab ? 2 : 1};

  adsb::ingest::BeastTcpSource source(src_cfg);
  auto source_b = b_beast ? std::make_unique<adsb::ingest::BeastTcpSource>(b_cfg) : nullptr;
  const std::uint64_t t_start = adsb::mono_ns();

  // One ingest thread per line. With A/B the state thread records (it sees
  // both lines in the merged order), so the recorder queue keeps one producer.
  auto ingest_line = [&](int line) {
    adsb::set_current_thread_name(line == 0 ? "adsb-ingest" : "adsb-ingest-b");
    const int cpu = line == 0 ? cpu_ingest : cpu_ingest_b;
    if (cpu >= 0) adsb::pin_current_thread(cpu);
    StateQueue& q = line == 0 ? *state_q : *state_qb;
    adsb::ingest::IqSource* iq = line == 0 ? iq_source.get() : iq_source_b.get();
    adsb::ingest::BeastTcpSource& beast = line == 0 ? source : *source_b;
    const adsb::ingest::BeastTcpConfig& bc = line == 0 ? src_cfg : b_cfg;
    const char* tag = !ab ? "" : line == 0 ? "line A " : "line B ";
    const std::size_t li = static_cast<std::size_t>(line);
    Item it{};
    std::size_t high_water = 0;
    const std::size_t queue_limit =
        max_queue == 0 || max_queue > StateQueue::kCapacity ? StateQueue::kCapacity : max_queue;
    auto on_record = [&](const adsb::record::Record& r) {
          if (rec_q && !ab) {
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
          const std::size_t depth = q.size_approx();
          if (depth > high_water) {
            high_water = depth;
            shared.line_high_water[li].store(depth, std::memory_order_relaxed);
            // The overall high water is the larger of the lines'.
            std::uint64_t cur = shared.queue_high_water.load(std::memory_order_relaxed);
            while (depth > cur && !shared.queue_high_water.compare_exchange_weak(cur, depth)) {
            }
          }
          auto drop = [&] {
            shared.overload_dropped.fetch_add(1, std::memory_order_relaxed);
            shared.line_dropped[li].fetch_add(1, std::memory_order_relaxed);
          };
          if (!overload_block && depth >= queue_limit) {
            drop();
            return;
          }
          while (!q.try_push(it)) {
            if (g_stop.load(std::memory_order_relaxed)) return;
            if (!overload_block) {  // full despite the check (limit == capacity)
              drop();
              return;
            }
            adsb::cpu_relax();
          }
        };
    if (iq) {
      iq->run(g_stop, on_record, [&](const char* status) {
        if (!quiet) std::fprintf(stderr, "adsb-live: %s%s: %s\n", tag, iq->config().path.c_str(), status);
      });
    } else {
      beast.run(g_stop, on_record, [&](const char* status) {
        if (!quiet) std::fprintf(stderr, "adsb-live: %s%s:%u %s\n", tag, bc.host.c_str(), bc.port, status);
      });
    }
    // The last line to finish ends the input. For IQ lines that is the end of
    // the files: stop like Ctrl-C (queues drain first). One atomic decision,
    // so two lines ending together cannot both leave it to the other.
    if (ingest_running.fetch_sub(1) == 1) {
      if (iq) g_stop.store(true);
      ingest_done.store(true, std::memory_order_release);
    }
  };
  std::thread ingest([&] { ingest_line(0); });
  std::thread ingest_b;
  if (ab) ingest_b = std::thread([&] { ingest_line(1); });

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
    // A/B: one frame of lookahead per line; the earlier receive time goes
    // first, so the arbiter (and the recording) see time order. A frame that
    // still arrives earlier than one already passed on (the other line's
    // thread was a little behind) has its time raised to keep the order.
    Item head[2]{};
    bool has[2] = {false, false};
    std::uint64_t last_rx = 0;
    std::uint64_t idle_spins = 0;
    for (;;) {
      if (!has[0]) has[0] = state_q->try_pop(head[0]);
      if (state_qb && !has[1]) has[1] = state_qb->try_pop(head[1]);
      if (has[0] || has[1]) {
        const int pick = has[0] && (!has[1] || head[0].rec.rx_ns <= head[1].rec.rx_ns) ? 0 : 1;
        Item& it = head[pick];
        has[pick] = false;
        idle_spins = 0;
        const std::uint64_t t_pop = adsb::ticks();
        auto& r = it.rec;
        bool forward = true;
        if (arbiter) {
          if (r.rx_ns < last_rx) {
            r.rx_ns = last_rx;
            shared.merge_clamped.fetch_add(1, std::memory_order_relaxed);
          }
          last_rx = r.rx_ns;
          if (rec_q) {
            if (rec_q->try_push(r)) {
              shared.recorded.fetch_add(1, std::memory_order_relaxed);
            } else {
              shared.record_dropped.fetch_add(1, std::memory_order_relaxed);
            }
          }
          forward = arbiter->offer(r);
          if (!forward) shared.arb_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        if (forward && (r.type == '2' || r.type == '3')) {  // Mode S only; Mode A/C carries no address
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
        // With A/B only forwarded frames count (a dropped copy updates nothing).
        const std::uint64_t done_ns = adsb::mono_ns();
        if (forward) recv_to_state->record(done_ns > r.rx_ns ? done_ns - r.rx_ns : 0);
        shared.frames.fetch_add(1, std::memory_order_relaxed);
        publish(engine->stats().bad_frames);
      } else {
        if (ingest_done.load(std::memory_order_acquire) && state_q->empty_approx() &&
            (!state_qb || state_qb->empty_approx())) {
          break;
        }
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
          // The writer refuses a record whose time goes backwards (or a bad
          // length); count it rather than lose it silently.
          if (!writer.append(r)) shared.record_rejected.fetch_add(1, std::memory_order_relaxed);
        } else if ((ab ? state_done : ingest_done).load(std::memory_order_acquire) && rec_q->empty_approx()) {
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
                  "%llu dropped (overload)%s%s\n",
                  static_cast<double>(now - t_start) * 1e-9, static_cast<unsigned long long>(f),
                  static_cast<double>(f - last_frames) / (static_cast<double>(now - last_t) * 1e-9),
                  static_cast<unsigned long long>(shared.tracks.load()),
                  static_cast<unsigned long long>(shared.positions.load()),
                  static_cast<unsigned long long>(shared.bad.load()),
                  static_cast<unsigned long long>(shared.overload_dropped.load()),
                  rec_q ? (", recorded " + std::to_string(shared.recorded.load())).c_str() : "",
                  ab ? (", " + std::to_string(shared.arb_dropped.load()) + " A/B copies dropped").c_str() : "");
      std::fflush(stdout);
      last_frames = f;
      last_t = now;
      next_report = now + static_cast<std::uint64_t>(stats_every_s * 1e9);
    }
  }

  ingest.join();
  if (ingest_b.joinable()) ingest_b.join();
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
  if (arbiter) arbiter->finish();
  const auto& es = engine->stats();
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::uint64_t input_frames = 0;
  auto report_line = [&](int line) {
    adsb::ingest::IqSource* iq = line == 0 ? iq_source.get() : iq_source_b.get();
    const adsb::ingest::IqSourceConfig& ic = line == 0 ? iq_cfg : iq_cfg_b;
    const char* tag = !ab ? "" : line == 0 ? " (line A)" : " (line B)";
    // "source      ..." as before for one line; "source (line A) ..." with A/B.
    auto lab = [&](const char* name) {
      std::string l = std::string(name) + tag;
      l.resize(std::max<std::size_t>(l.size() + 1, 12), ' ');
      return l;
    };
    if (iq && iq->device_mode()) {
      const auto q = iq->stats();
      input_frames += q.frames;
      std::printf("%sRTL-SDR %s: %s; %zu-sample transfers, %zu buffers%s%s\n", lab("source").c_str(), ic.path.c_str(),
                  adsb::ingest::rtl_info_text(iq->device_info()).c_str(), ic.block_samples, ic.buffers,
                  q.read_error ? " DEVICE ERROR: " : "", q.read_error ? iq->error_text().c_str() : "");
      std::printf("%s%llu transfers (%llu samples, %.2f s), %llu transfers dropped (%llu samples, %.4f%%), "
                  "ring high water %llu of %zu\n",
                  lab("iq").c_str(), u(q.blocks), u(q.samples), static_cast<double>(q.samples) / ic.sample_rate_hz,
                  u(q.blocks_dropped), u(q.samples_dropped),
                  q.samples ? 100.0 * static_cast<double>(q.samples_dropped) / static_cast<double>(q.samples) : 0.0,
                  u(q.ring_high_water), ic.buffers);
      std::printf("%s%llu frames (%llu by the sequence detector, %llu fixed), %llu preambles, %llu failed; "
                  "%llu frame times raised to stay monotonic\n",
                  lab("demod").c_str(), u(q.demod.frames), u(q.demod.sequence_decoded), u(q.demod.corrected), u(q.demod.preambles),
                  u(q.demod.bad_crc), u(q.time_clamped));
    } else if (iq) {
      const auto q = iq->stats();
      input_frames += q.frames;
      std::printf("%sIQ %s (%s, %.3f MS/s, %zu-sample blocks, %zu buffers, %s)%s\n", lab("source").c_str(), ic.path.c_str(),
                  adsb::demod::format_name(ic.format), ic.sample_rate_hz / 1e6, ic.block_samples, ic.buffers,
                  ic.speed > 0 ? (std::to_string(ic.speed) + "x real time").c_str() : "unpaced",
                  q.read_error ? " READ ERROR" : "");
      std::printf("%s%llu blocks (%llu samples, %.2f s), %llu blocks dropped (%llu samples, %.4f%%), "
                  "ring high water %llu of %zu, max lateness %.3f ms\n",
                  lab("iq").c_str(), u(q.blocks), u(q.samples), static_cast<double>(q.samples) / ic.sample_rate_hz,
                  u(q.blocks_dropped), u(q.samples_dropped),
                  q.samples ? 100.0 * static_cast<double>(q.samples_dropped) / static_cast<double>(q.samples) : 0.0,
                  u(q.ring_high_water), ic.buffers, static_cast<double>(q.max_lateness_ns) * 1e-6);
      std::printf("%s%llu frames (%llu by the sequence detector, %llu fixed), %llu preambles, %llu failed; "
                  "echoes: %llu frames moved to their direct path, %llu duplicates dropped; "
                  "%llu frame times raised to stay monotonic\n",
                  lab("demod").c_str(), u(q.demod.frames), u(q.demod.sequence_decoded), u(q.demod.corrected), u(q.demod.preambles),
                  u(q.demod.bad_crc), u(q.demod.retimed), u(q.demod.duplicates), u(q.time_clamped));
    } else {
      const auto st = line == 0 ? source.stats() : source_b->stats();
      const adsb::ingest::BeastTcpConfig& bc = line == 0 ? src_cfg : b_cfg;
      input_frames += st.frames;
      std::printf("%s%s:%u, %llu connects, %llu disconnects, %llu failed attempts\n", lab("source").c_str(), bc.host.c_str(),
                  bc.port, u(st.connects), u(st.disconnects), u(st.connect_failures));
      std::printf("%s%llu bytes, %llu frames, %llu truncated, %llu bad type, %llu bytes skipped\n", lab("stream").c_str(),
                  u(st.bytes), u(st.frames), u(st.parser.truncated), u(st.parser.bad_type),
                  u(st.parser.skipped_bytes));
    }
  };
  report_line(0);
  if (ab) {
    report_line(1);
    std::fputs(adsb::ingest::arbiter_report(*arbiter).c_str(), stdout);
    std::printf("merge       %llu receive times raised to keep the two lines in order\n",
                u(shared.merge_clamped.load()));
  }
  std::printf("engine      %llu frames, %llu bad, %llu duplicates, A/P accepted %llu / rejected %llu\n",
              u(es.frames), u(es.bad_frames), u(es.duplicates), u(es.ap_accepted), u(es.ap_rejected));
  if (eng_cfg.fix_single_bit) std::printf("corrected   %llu frames (one bit repaired), %llu of them dropped (no confirmed track)\n", u(es.corrected), u(es.corrected_rejected));
  std::printf("positions   global %llu, local %llu, unresolved %llu, rejected %llu\n", u(es.cpr_global),
              u(es.cpr_local), u(es.cpr_unresolved), u(es.cpr_rejected));
  std::printf("tracks      created %llu, expired %llu, live %zu\n", u(es.tracks_created), u(es.tracks_expired),
              engine->track_count());
  if (!record_path.empty()) {
    std::printf("recording   %s: %llu records, %llu dropped, %llu rejected by the writer%s\n", record_path.c_str(),
                u(writer.count()), u(shared.record_dropped.load()), u(shared.record_rejected.load()),
                rec_ok ? "" : " (WRITE FAILED)");
  }
  if (udp) {
    (void)udp->drain();
    if (const std::string r = udp->report(); !r.empty()) std::printf("%s\n", r.c_str());
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
              "queue high water %llu of %zu%s\n",
              overload_block ? "block" : "drop", u(shared.overload_dropped.load()),
              input_frames ? 100.0 * static_cast<double>(shared.overload_dropped.load()) / static_cast<double>(input_frames) : 0.0,
              u(shared.queue_high_water.load()),
              max_queue == 0 || max_queue > StateQueue::kCapacity ? StateQueue::kCapacity : max_queue,
              ab ? ("; line A " + std::to_string(shared.line_dropped[0].load()) + " dropped, high water " +
                    std::to_string(shared.line_high_water[0].load()) + "; line B " +
                    std::to_string(shared.line_dropped[1].load()) + " dropped, high water " +
                    std::to_string(shared.line_high_water[1].load()))
                       .c_str()
                 : "");
  std::printf("build       %s\n", adsb::build_variant());
  std::printf("latency (receive -> track state updated%s, per frame):\n", pub ? " and published" : "");
  print_hist("queue wait", *queue_wait);
  print_hist(iq_mode ? "last sample -> state" : "receive -> state done", *recv_to_state);
  // An IQ input that failed (a read error, a device error or a lost device)
  // fails the run.
  bool iq_failed = false;
  for (adsb::ingest::IqSource* iq : {iq_source.get(), iq_source_b.get()}) {
    if (iq && iq->stats().read_error) {
      std::fprintf(stderr, "adsb-live: %s: %s\n", iq->config().path.c_str(),
                   iq->error_text().empty() ? "read error" : iq->error_text().c_str());
      iq_failed = true;
    }
  }
  return rec_ok && !conf_failed && !iq_failed ? 0 : 1;
}
