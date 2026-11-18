// adsb-shm-cli: subscribe to the track feed on the same host through the
// shared-memory ring (README_UPDATES.md §9.3), the shm counterpart of
// adsb-feed-cli. Joins from the ring (or a snapshot), follows it, recovers
// from laps and publisher restarts, and reports.
//
//   adsb-shm-cli [--name /adsb-feed] [--snapshot HOST:PORT] [--until-end]
//                [--duration S] [--stats-every S] [--slow-ns N] [--idle-sleep-us N]
//
// The last line printed is the digest of the reconstructed book; after a
// complete session it equals the publisher's "book digest" line.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/shm_subscriber.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-shm-cli [options]\n"
               "  --name NAME           shared-memory ring (default /adsb-feed)\n"
               "  --snapshot HOST:PORT  snapshot server, for joining late and recovering from laps\n"
               "                        (default 127.0.0.1:41902)\n"
               "  --until-end           exit once the session has ended and everything is applied\n"
               "  --duration S          exit after S seconds\n"
               "  --stats-every S       print counters every S seconds (default 0: only at the end)\n"
               "  --slow-ns N           spin N ns after every message (a slow consumer, to provoke laps)\n"
               "  --idle-sleep-us N     sleep instead of spinning when there is nothing to read\n"
               "                        (default 0: spin, lowest latency, uses a core)\n"
               "  --quiet               only the final lines\n");
  std::exit(code);
}

class Report : public SubscriberHandler {
 public:
  Report(const ShmSubscriber* s, double every_s, std::uint64_t slow_ns, bool quiet)
      : s_(s), every_ns_(static_cast<std::uint64_t>(every_s * 1e9)), slow_ns_(slow_ns), quiet_(quiet) {}
  void on_message(std::uint64_t, std::span<const std::uint8_t> msg) override {
    // publish_ts_ns @56: the publisher's mono_ns(), comparable on the same host.
    if (msg.size() >= protocol::TrackUpdate::kWireSize &&
        msg[0] == static_cast<std::uint8_t>(protocol::TrackUpdate::kTypeChar)) {
      std::uint64_t pub_ts;
      std::memcpy(&pub_ts, msg.data() + 56, 8);
      const std::uint64_t now = mono_ns();
      // Messages published before we went live were read as catch-up (after a
      // join or a resync): counted apart, so the histogram is live latency.
      if (pub_ts != 0 && now >= pub_ts) {
        if (pub_ts >= s_->live_since_ns()) latency_.record(now - pub_ts);
        else ++catch_up_;
      }
    }
    if (slow_ns_) {
      const std::uint64_t until = mono_ns() + slow_ns_;
      while (mono_ns() < until) {}
    }
  }
  void on_reset() override {
    if (!quiet_) {
      const auto s = s_->session();
      std::printf("session     %.10s\n", s.data());
      std::fflush(stdout);
    }
  }
  void on_snapshot_loaded() override {
    if (!quiet_) {
      std::printf("snapshot    loaded: %zu tracks, resuming at seq %llu\n", s_->book().size(),
                  static_cast<unsigned long long>(s_->expected()));
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
    const auto& st = s_->stats();
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("stats       seq %llu, %zu tracks, %llu applied, %llu laps, %llu snapshots (%llu failed), "
                "%llu rings opened\n",
                u(s_->expected() ? s_->expected() - 1 : 0), s_->book().size(), u(st.applied), u(st.laps),
                u(st.snapshots_loaded), u(st.snapshot_failures), u(st.rings_opened));
    std::fflush(stdout);
  }
  [[nodiscard]] const LatencyHistogram& latency() const { return latency_; }
  [[nodiscard]] std::uint64_t catch_up() const { return catch_up_; }

 private:
  const ShmSubscriber* s_;
  std::uint64_t catch_up_ = 0;
  std::uint64_t every_ns_;
  std::uint64_t slow_ns_;
  bool quiet_;
  std::uint64_t next_ = 0;
  LatencyHistogram latency_;
};

}  // namespace

int main(int argc, char** argv) {
  ShmSubscriberConfig cfg;
  double duration_s = 0.0, stats_every_s = 0.0;
  std::uint64_t slow_ns = 0;
  unsigned idle_sleep_us = 0;
  bool until_end = false, quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--name") cfg.name = val();
    else if (a == "--snapshot") { if (!parse_endpoint(val(), cfg.snapshot)) usage(1); }
    else if (a == "--until-end") until_end = true;
    else if (a == "--duration") duration_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--stats-every") stats_every_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--slow-ns") slow_ns = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--idle-sleep-us") idle_sleep_us = static_cast<unsigned>(std::strtoul(val().c_str(), nullptr, 10));
    else if (a == "--quiet") quiet = true;
    else usage(1);
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  ShmSubscriber sub(cfg);
  Report report(&sub, stats_every_s, slow_ns, quiet);
  sub.set_handler(&report);
  std::string err;
  if (!sub.open(&err) && !quiet) {
    std::fprintf(stderr, "adsb-shm-cli: %s (waiting for the publisher)\n", err.c_str());
  }
  std::thread timer;
  if (duration_s > 0) {
    timer = std::thread([duration_s] {
      const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration_s);
      while (!g_stop.load() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      g_stop.store(true);
    });
  }
  sub.run(g_stop, until_end, idle_sleep_us);
  g_stop.store(true);
  if (timer.joinable()) timer.join();

  report.print_counters();
  const auto& h = report.latency();
  if (h.count() > 0) {
    std::printf("latency     publish -> applied (same host, shm, live): p50 %llu  p99 %llu  p99.9 %llu  max %llu ns "
                "(n=%llu; %llu catch-up messages after a join or resync not counted)\n",
                static_cast<unsigned long long>(h.percentile(50)), static_cast<unsigned long long>(h.percentile(99)),
                static_cast<unsigned long long>(h.percentile(99.9)), static_cast<unsigned long long>(h.max()),
                static_cast<unsigned long long>(h.count()), static_cast<unsigned long long>(report.catch_up()));
  }
  const bool done = sub.ended();
  std::printf("session     %.10s, %s\n", sub.session().data(),
              done ? "ended" : sub.gave_up() ? "NOT complete: gave up (lapped after the end, no snapshot)" : "not ended");
  std::printf("book digest %s\n", sub.book().hex_digest().c_str());
  return until_end && !done ? 3 : 0;
}
