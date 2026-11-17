// adsb-feed-cli: subscribe to the track feed, verify sequencing, recover
// gaps (retransmit, then snapshot), and report (README_UPDATES.md §9.6).
//
//   adsb-feed-cli [--feed 239.19.0.1:41900] [--interface ADDR]
//                 [--retransmit HOST:PORT] [--snapshot HOST:PORT]
//                 [--until-end] [--duration S] [--drop-rate P] ...
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
  bool until_end = false, quiet = false;
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
    else if (a == "--quiet") quiet = true;
    else { std::fprintf(stderr, "adsb-feed-cli: unknown option %s\n", a.c_str()); usage(1); }
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
  if (!client.last_snapshot_error().empty()) {
    std::printf("snapshot    last error: %s\n", client.last_snapshot_error().c_str());
  }
  const LatencyHistogram& h = report.latency();
  if (h.count() > 0) {
    std::printf("latency     publish -> applied (same host): p50 %llu  p99 %llu  p99.9 %llu  max %llu ns\n",
                u(h.percentile(50)), u(h.percentile(99)), u(h.percentile(99.9)), u(h.max()));
  }
  std::printf("book        %zu tracks\n", core.book().size());
  std::printf("book digest %s\n", core.book().hex_digest().c_str());
  return until_end && !core.ended() ? 3 : 0;
}
