// adsb-feed-cli: subscribe to the track feed, verify sequencing, recover
// gaps (retransmit, then snapshot), and report (README_UPDATES.md §9.6).
//
//   adsb-feed-cli [--feed 239.19.0.1:41900] [--interface ADDR]
//                 [--retransmit HOST:PORT] [--snapshot HOST:PORT]
//                 [--until-end] [--duration S] [--drop-rate P] ...
//                 [--rx kernel|xdp] [--xdp-queue N] [--xdp-mode M] [--xdp-attach A]
//
// The last line printed is the digest of the reconstructed book; after a
// complete session it equals the publisher's "book digest" line.

#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/subscriber.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-feed-cli [options]\n"
               "  --feed HOST:PORT        multicast group or local unicast address (default 239.19.0.1:41900)\n"
               "  --interface ADDR        interface to join the group on (default: kernel's choice)\n"
               "  --retransmit HOST:PORT  retransmit server (default 127.0.0.1:41901)\n"
               "  --snapshot HOST:PORT    snapshot server (default 127.0.0.1:41902)\n"
               "  --until-end             exit once the session has ended and everything is recovered\n"
               "  --duration S            exit after S seconds (default: until Ctrl-C or --until-end)\n"
               "  --stats-every S         print counters every S seconds (default 0 = only at the end)\n"
               "  --window N              messages buffered ahead of a gap (default 16384)\n"
               "  --max-request N         messages per retransmit request (default 1024)\n"
               "  --drop-rate P           fault injection: drop received feed datagrams\n"
               "  --drop-burst N          datagrams per drop burst (default 1)\n"
               "  --seed N                fault injection seed (default 1)\n"
               "  --drop-only-first N     inject only into the first N feed datagrams\n"
               "  --rx kernel|xdp         feed reception: kernel socket (default) or AF_XDP (an XDP program\n"
               "                          redirects the feed into an XSK on --interface's device; needs\n"
               "                          CAP_NET_ADMIN, CAP_BPF, CAP_NET_RAW; while it runs no other local\n"
               "                          socket on that device receives the feed: one per device)\n"
               "  --xdp-queue N           with --rx xdp: the device RX queue to bind (default 0)\n"
               "  --xdp-mode M            with --rx xdp: auto, copy or zerocopy (default auto)\n"
               "  --xdp-attach A          with --rx xdp: auto, driver or generic (default auto)\n"
               "  --xdp-frames N          with --rx xdp: UMEM frames, power of two (default 2048)\n"
               "  --quiet                 only the final report\n");
  std::exit(code);
}

std::uint64_t parse_u64(const std::string& v) {
  char* end = nullptr;
  const unsigned long long x = std::strtoull(v.c_str(), &end, 10);
  if (v.empty() || *end != '\0') usage(1);
  return x;
}

// Runs on the subscriber thread: latency and periodic stats.
class Report : public SubscriberHandler {
 public:
  Report(SubscriberClient& c, double every_s, bool quiet) : c_(c), every_ns_(static_cast<std::uint64_t>(every_s * 1e9)), quiet_(quiet) {}
  void on_message(std::uint64_t, std::span<const std::uint8_t> msg) override {
    // publish_ts_ns @56: publisher's monotonic clock, comparable on the same host only.
    if (msg.size() >= protocol::TrackUpdate::kWireSize &&
        msg[0] == static_cast<std::uint8_t>(protocol::TrackUpdate::kTypeChar)) {
      std::uint64_t pub_ts;
      std::memcpy(&pub_ts, msg.data() + 56, 8);
      const std::uint64_t now = mono_ns();
      if (pub_ts != 0 && now >= pub_ts) latency_.record(now - pub_ts);
    }
  }
  void on_reset() override {
    if (!quiet_ && c_.core().has_session()) {
      const auto& s = c_.core().session();
      std::printf("session     %.10s\n", s.data());
      std::fflush(stdout);
    }
  }
  void on_snapshot_loaded() override {
    if (!quiet_) {
      std::printf("snapshot    loaded: %zu tracks, resuming at seq %llu\n", c_.core().book().size(),
                  static_cast<unsigned long long>(c_.core().expected()));
      std::fflush(stdout);
    }
  }
  void on_tick(std::uint64_t now) override {
    if (every_ns_ == 0) return;
    if (next_ == 0) next_ = now + every_ns_;
    if (now < next_) return;
    next_ += every_ns_;
    print_counters();
  }
  void print_counters() const {
    const auto& s = c_.core().stats();
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("stats       seq %llu, %zu tracks, %llu applied, %llu gaps, %llu requests (%llu retries), "
                "%llu NAKs, %llu snapshots\n",
                u(c_.core().expected() - 1), c_.core().book().size(), u(s.applied), u(s.gaps),
                u(s.requests), u(s.request_retries), u(s.naks), u(s.snapshots_loaded));
    std::fflush(stdout);
  }
  [[nodiscard]] const LatencyHistogram& latency() const { return latency_; }

 private:
  SubscriberClient& c_;
  std::uint64_t every_ns_;
  bool quiet_;
  std::uint64_t next_ = 0;
  LatencyHistogram latency_;
};

}  // namespace

int main(int argc, char** argv) {
  SubscriberClientConfig cfg;
  double duration_s = 0.0, stats_every_s = 0.0;
  bool until_end = false, quiet = false, xdp_flags = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--feed") { if (!parse_endpoint(val(), cfg.feed)) usage(1); }
    else if (a == "--interface") cfg.interface_host = val();
    else if (a == "--retransmit") { if (!parse_endpoint(val(), cfg.retransmit)) usage(1); }
    else if (a == "--snapshot") { if (!parse_endpoint(val(), cfg.snapshot)) usage(1); }
    else if (a == "--until-end") until_end = true;
    else if (a == "--duration") duration_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--stats-every") stats_every_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--window") {
      const std::uint64_t n = parse_u64(val());
      if (n == 0 || n > SubscriberConfig::kMaxWindow) usage(1);
      cfg.core.window = static_cast<std::uint32_t>(n);
    }
    else if (a == "--max-request") {
      const std::uint64_t n = parse_u64(val());
      if (n == 0 || n > 65535) usage(1);
      cfg.core.max_request = static_cast<std::uint16_t>(n);
    } else if (a == "--drop-rate") cfg.feed_loss.drop_rate = std::strtod(val().c_str(), nullptr);
    else if (a == "--drop-burst") cfg.feed_loss.drop_burst = static_cast<std::uint32_t>(parse_u64(val()));
    else if (a == "--seed") cfg.feed_loss.seed = parse_u64(val());
    else if (a == "--drop-only-first") cfg.feed_loss.only_first = parse_u64(val());
    else if (a == "--rx") {
      const std::string v = val();
      if (v == "kernel") cfg.rx = SubscriberClientConfig::Rx::kKernel;
      else if (v == "xdp") cfg.rx = SubscriberClientConfig::Rx::kXdp;
      else usage(1);
    } else if (a.rfind("--xdp-", 0) == 0) {
      xdp_flags = true;
      if (a == "--xdp-queue") {
        const std::uint64_t q = parse_u64(val());
        if (q > 1023) usage(1);
        cfg.xdp.queue = static_cast<unsigned>(q);
      } else if (a == "--xdp-mode") { if (!XdpSender::parse_mode(val(), cfg.xdp.mode)) usage(1); }
      else if (a == "--xdp-attach") { if (!XdpReceiver::parse_attach(val(), cfg.xdp.attach)) usage(1); }
      else if (a == "--xdp-frames") {
        const std::uint64_t n = parse_u64(val());
        if (n < 64 || n > 65536) usage(1);
        cfg.xdp.frames = static_cast<unsigned>(n);
      } else usage(1);
    }
    else if (a == "--quiet") quiet = true;
    else { std::fprintf(stderr, "adsb-feed-cli: unknown option %s\n", a.c_str()); usage(1); }
  }

  if (cfg.rx != SubscriberClientConfig::Rx::kXdp && xdp_flags) {
    std::fprintf(stderr, "adsb-feed-cli: --xdp-* options need --rx xdp\n");
    return 1;
  }
  SubscriberClient client(cfg);
  std::string err;
  if (!client.open(&err)) {
    std::fprintf(stderr, "adsb-feed-cli: %s\n", err.c_str());
    return 1;
  }
  Report report(client, stats_every_s, quiet);
  client.core().set_handler(&report);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  if (!quiet) {
    std::printf("listening   feed %s  retransmit %s  snapshot %s\n", to_string(cfg.feed).c_str(),
                to_string(cfg.retransmit).c_str(), to_string(cfg.snapshot).c_str());
    if (client.xdp().is_open()) {
      const XdpReceiver& x = client.xdp();
      std::printf("af_xdp      %s queue %u, %s mode, program attached in %s mode\n", x.device().name.c_str(),
                  cfg.xdp.queue, XdpSender::mode_name(x.bound_mode()), XdpReceiver::attach_name(x.attach_mode()));
    }
    std::fflush(stdout);
  }

  std::thread timer;
  if (duration_s > 0.0) {
    timer = std::thread([duration_s] {
      const std::uint64_t end = mono_ns() + static_cast<std::uint64_t>(duration_s * 1e9);
      while (!g_stop.load() && mono_ns() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      g_stop.store(true);
    });
  }
  client.run(g_stop, until_end);
  g_stop.store(true);
  if (timer.joinable()) timer.join();

  const auto& core = client.core();
  const auto& s = core.stats();
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("session     %.10s%s\n", core.has_session() ? core.session().data() : "(none)",
              core.ended() ? ", ended and fully recovered" : "");
  std::printf("received    %llu feed datagrams (%llu dropped by injection), %llu retransmit datagrams, "
              "%llu heartbeats, %llu malformed\n",
              u(s.feed_packets), u(client.feed_dropped()), u(s.retransmit_packets), u(s.heartbeats),
              u(s.malformed));
  std::printf("sequencing  %llu applied, %llu duplicates, %llu buffered, %llu overflow, next seq %llu, "
              "known %llu\n",
              u(s.applied), u(s.duplicates), u(s.buffered), u(s.window_overflow), u(core.expected()),
              u(core.known_next() - 1));
  std::printf("recovery    %llu gaps, %llu requests (%llu retries), %llu NAKs, %llu snapshots loaded "
              "(%llu failed), %llu session changes, %llu stale\n",
              u(s.gaps), u(s.requests), u(s.request_retries), u(s.naks), u(s.snapshots_loaded),
              u(s.snapshot_failures), u(s.session_changes), u(s.stale_packets));
  if (client.xdp().is_open()) {
    XdpReceiver& x = client.xdp();
    x.refresh_kernel_stats();
    const auto& xs = x.stats();
    std::printf("rx          af_xdp %llu, kernel %llu feed datagrams; af_xdp %s/%s: %llu rejected, %llu wakeups, "
                "kernel drops %llu (ring full %llu, fill empty %llu, invalid %llu)\n",
                u(client.feed_via_xdp()), u(client.feed_via_kernel()), XdpSender::mode_name(x.bound_mode()),
                XdpReceiver::attach_name(x.attach_mode()), u(xs.rejected), u(xs.wakeups), u(xs.rx_dropped),
                u(xs.rx_ring_full), u(xs.rx_fill_ring_empty), u(xs.rx_invalid));
    if (client.feed_via_xdp() == 0 && client.feed_via_kernel() > 0) {
      std::printf("warning     no feed datagram came through AF_XDP: on a multi-queue NIC the feed may land on a "
                  "queue other than %u (steer it there with ethtool -N, or pick that queue)\n", cfg.xdp.queue);
    }
  }
  if (!client.last_snapshot_error().empty()) {
    std::printf("snapshot    last error: %s\n", client.last_snapshot_error().c_str());
  }
  const LatencyHistogram& h = report.latency();
  if (h.count() > 0) {
    std::printf("latency     publish -> applied (same host): p50 %llu  p99 %llu  p99.9 %llu  max %llu ns\n",
                u(h.percentile(50)), u(h.percentile(99)), u(h.percentile(99.9)), u(h.max()));
  }
  std::printf("book        %zu tracks\n", core.book().size());
  std::printf("state digest %s\n", core.book().hex_state_digest().c_str());
  std::printf("book digest %s\n", core.book().hex_digest().c_str());
  return until_end && !core.ended() ? 3 : 0;
}
