// adsb-publish: replay a recording through decode + track state and publish
// the result as the sequenced UDP feed, with the retransmit and snapshot
// servers running alongside (README_UPDATES.md §9).
//
//   adsb-publish FILE.adsbrec [--feed 239.19.0.1:41900] [--interface ADDR]
//                [--speed max|N] [--batch-us N] [--drop-rate P] [--linger S] ...
//
// Threads: the main thread replays (paced), runs the engine and publishes;
// a second thread runs the recovery servers. When the recording ends the
// session is ended, and the recovery servers keep answering for --linger
// seconds so subscribers can fill the tail.
//
// The last line printed is the digest of the publisher's final book, which a
// subscriber that recovered correctly reproduces exactly (adsb-feed-cli).

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "adsb/common/build_config.hpp"
#include "adsb/common/clock.hpp"
#include "adsb/common/thread.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/replay/replay.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-publish FILE.adsbrec [options]\n"
               "feed:\n"
               "  --feed HOST:PORT        destination: multicast group or unicast address (default 239.19.0.1:41900)\n"
               "  --interface ADDR        multicast egress interface address (default: kernel's choice)\n"
               "  --ttl N                 multicast TTL (default 1: stays on the LAN)\n"
               "  --session ID            session id, up to 10 characters; must differ on every start (default: generated)\n"
               "  --batch-us N            batch up to N microseconds per datagram (default 0: one message per datagram)\n"
               "  --heartbeat-ms N        heartbeat after N ms without traffic (default 1000)\n"
               "  --no-publish-ts         leave TrackUpdate.publish_ts_ns at 0 (byte-deterministic feed)\n"
               "recovery:\n"
               "  --bind ADDR             address for the recovery servers (default 0.0.0.0)\n"
               "  --retransmit-port P     UDP retransmit server (default 41901)\n"
               "  --snapshot-port P       TCP snapshot server (default 41902)\n"
               "  --ring N                messages held for retransmission (default 65536)\n"
               "  --linger S              keep serving recovery for S seconds after the end (default 2)\n"
               "replay:\n"
               "  --speed max|N           max, or N x original speed (default 1 = real time)\n"
               "  --busy-wait             pace by spinning (precise timing, uses a core)\n"
               "  --no-flush              do not expire the remaining tracks at the end\n"
               "  --cpus P,R              pin publisher and recovery threads (Linux only)\n"
               "fault injection:\n"
               "  --drop-rate P           probability a feed drop burst starts at a datagram\n"
               "  --drop-burst N          datagrams per drop burst (default 1)\n"
               "  --reply-drop-rate P     probability a retransmit reply datagram is dropped\n"
               "  --seed N                fault injection seed (default 1)\n"
               "  --drop-only-first N     inject only into the first N feed datagrams\n");
  std::exit(code);
}

std::uint64_t parse_u64(const std::string& v) {
  char* end = nullptr;
  const unsigned long long x = std::strtoull(v.c_str(), &end, 10);
  if (v.empty() || *end != '\0') usage(1);
  return x;
}
double parse_prob(const std::string& v) {
  char* end = nullptr;
  const double x = std::strtod(v.c_str(), &end);
  if (v.empty() || *end != '\0' || x < 0.0 || x > 1.0) usage(1);
  return x;
}
std::uint16_t parse_port(const std::string& v) {
  const std::uint64_t p = parse_u64(v);
  if (p > 65535) usage(1);
  return static_cast<std::uint16_t>(p);
}

// Fans the engine's output into the publisher and counts it.
template <typename Pub>
struct PublishSink {
  Pub* pub;
  std::uint64_t updates = 0, deletes = 0;
  void on_update(const protocol::TrackUpdate& u) {
    ++updates;
    pub->on_update(u);
  }
  void on_delete(const protocol::TrackDelete& d) {
    ++deletes;
    pub->on_delete(d);
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  Endpoint feed{"239.19.0.1", 41900};
  UdpSender::Options send_opt;
  std::string session_id;
  PublisherConfig pcfg;
  RecoveryServerConfig rcfg;
  rcfg.retransmit_port = 41901;
  rcfg.snapshot_port = 41902;
  LossConfig feed_loss;
  double speed = 1.0, linger_s = 2.0;
  bool busy_wait = false, flush_at_end = true;
  int pub_cpu = -1, rec_cpu = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--feed") { if (!parse_endpoint(val(), feed)) usage(1); }
    else if (a == "--interface") send_opt.interface_host = val();
    else if (a == "--ttl") send_opt.ttl = static_cast<int>(parse_u64(val()) & 0xFF);
    else if (a == "--session") { session_id = val(); if (session_id.empty() || session_id.size() > 10) usage(1); }
    else if (a == "--batch-us") pcfg.max_batch_delay_ns = parse_u64(val()) * 1000;
    else if (a == "--heartbeat-ms") { pcfg.heartbeat_interval_ns = parse_u64(val()) * 1'000'000; if (!pcfg.heartbeat_interval_ns) usage(1); }
    else if (a == "--no-publish-ts") pcfg.stamp_publish_ts = false;
    else if (a == "--bind") rcfg.bind_host = val();
    else if (a == "--retransmit-port") rcfg.retransmit_port = parse_port(val());
    else if (a == "--snapshot-port") rcfg.snapshot_port = parse_port(val());
    else if (a == "--ring") {
      rcfg.recovery.ring_capacity = parse_u64(val());
      if (!rcfg.recovery.ring_capacity || rcfg.recovery.ring_capacity > RetransmitStore::kMaxCapacity) usage(1);
    }
    else if (a == "--linger") linger_s = std::strtod(val().c_str(), nullptr);
    else if (a == "--speed") {
      const std::string v = val();
      speed = v == "max" ? 0.0 : std::strtod(v.c_str(), nullptr);
      if (v != "max" && speed <= 0.0) usage(1);
    } else if (a == "--busy-wait") busy_wait = true;
    else if (a == "--no-flush") flush_at_end = false;
    else if (a == "--cpus") { if (std::sscanf(val().c_str(), "%d,%d", &pub_cpu, &rec_cpu) != 2) usage(1); }
    else if (a == "--drop-rate") feed_loss.drop_rate = parse_prob(val());
    else if (a == "--drop-burst") feed_loss.drop_burst = static_cast<std::uint32_t>(parse_u64(val()));
    else if (a == "--reply-drop-rate") rcfg.reply_loss.drop_rate = parse_prob(val());
    else if (a == "--drop-only-first") feed_loss.only_first = parse_u64(val());
    else if (a == "--seed") { feed_loss.seed = parse_u64(val()); rcfg.reply_loss.seed = feed_loss.seed + 1; }
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "adsb-publish: unknown option %s\n", a.c_str()); usage(1); }
    else if (path.empty()) path = a;
    else usage(1);
  }
  if (path.empty()) usage(1);
  if (session_id.empty()) {
    const protocol::Session s = unique_session('P');
    session_id.assign(s.begin(), s.end());
  }
  const protocol::Session session = protocol::make_session(session_id.c_str());
  pcfg.session = session;
  rcfg.recovery.session = session;

  record::RecordingReader rd;
  std::string err;
  if (!rd.open(path, &err)) {
    std::fprintf(stderr, "adsb-publish: %s\n", err.c_str());
    return 1;
  }
  state::EngineConfig ecfg;
  if (rd.info().has_receiver_position) {
    ecfg.has_receiver = true;
    ecfg.receiver_lat = rd.info().receiver_lat;
    ecfg.receiver_lon = rd.info().receiver_lon;
  }
  auto engine = std::make_unique<DefaultTrackEngine>(ecfg);

  UdpSender udp;
  if (!udp.open(feed, send_opt, &err)) {
    std::fprintf(stderr, "adsb-publish: feed %s: %s\n", to_string(feed).c_str(), err.c_str());
    return 1;
  }
  auto log = make_message_log();
  RecoveryServer recovery(rcfg, *log);
  if (!recovery.open(&err)) {
    std::fprintf(stderr, "adsb-publish: recovery servers: %s\n", err.c_str());
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::atomic<bool> rec_stop{false};
  std::thread rec_thread([&] {
    if (rec_cpu >= 0) pin_current_thread(rec_cpu);
    recovery.run(rec_stop);
  });
  if (pub_cpu >= 0) pin_current_thread(pub_cpu);

  LossyOut<UdpSender> out(udp, feed_loss);
  Publisher<LossyOut<UdpSender>> pub(pcfg, out, log.get());
  PublishSink<Publisher<LossyOut<UdpSender>>> sink{&pub};

  std::printf("session     %s  feed %s  retransmit %s:%u  snapshot %s:%u\n", session_id.c_str(),
              to_string(feed).c_str(), rcfg.bind_host.c_str(), recovery.retransmit_port(),
              rcfg.bind_host.c_str(), recovery.snapshot_port());
  std::fflush(stdout);

  // Replay, publishing as we go; poll the publisher while waiting so batches
  // flush on time and heartbeats go out during quiet periods.
  record::Record r;
  std::uint64_t first_rx = 0, last_rx = 0, records = 0;
  const std::uint64_t wall0 = mono_ns();
  while (!g_stop.load(std::memory_order_relaxed) && rd.next(r)) {
    if (records == 0) first_rx = r.rx_ns;
    if (speed > 0.0) {
      const std::uint64_t target =
          wall0 + static_cast<std::uint64_t>(static_cast<double>(r.rx_ns - first_rx) / speed);
      for (;;) {
        const std::uint64_t now = mono_ns();
        if (now >= target) break;
        pub.poll();
        if (!busy_wait && target - now > 2'000'000) {
          timespec ts{0, 500'000};
          nanosleep(&ts, nullptr);
        } else {
          cpu_relax();
        }
      }
    }
    engine->on_frame(r.payload, r.len, replay::detail::meta_of(r), sink);
    pub.poll();
    last_rx = r.rx_ns;
    ++records;
  }
  if (flush_at_end && last_rx != 0 && !g_stop.load()) {
    engine->advance(last_rx + engine->config().track_timeout_ns + 2'000'000'000ULL, sink);
  }
  pub.end_session();
  const double wall = static_cast<double>(mono_ns() - wall0) * 1e-9;

  // Linger: keep repeating the end-of-session marker and serving recovery.
  const std::uint64_t linger_end = mono_ns() + static_cast<std::uint64_t>(linger_s * 1e9);
  while (!g_stop.load() && mono_ns() < linger_end) {
    pub.poll();
    timespec ts{0, 1'000'000};
    nanosleep(&ts, nullptr);
  }
  rec_stop = true;
  rec_thread.join();

  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  const auto& ps = pub.stats();
  const auto& rc = recovery.counters();
  const auto& rs = recovery.state().stats();
  std::printf("build       %s\n", build_variant());
  std::printf("replayed    %llu records in %.3f s\n", u(records), wall);
  std::printf("engine      %llu updates, %llu deletes\n", u(sink.updates), u(sink.deletes));
  std::printf("published   %llu messages in %llu datagrams (%llu bytes), %llu heartbeats, last seq %llu\n",
              u(ps.messages), u(ps.packets), u(ps.bytes), u(ps.heartbeats), u(pub.next_seq() - 1));
  std::printf("faults      %llu feed datagrams dropped, %llu replies dropped, %llu send failures\n",
              u(out.dropped()), u(rc.replies_dropped.load()), u(ps.send_failures));
  std::printf("recovery    %llu requests, %llu messages retransmitted, %llu NAKs, %llu snapshots, ring %zu\n",
              u(rc.requests.load()), u(rc.served_messages.load()), u(rc.naks.load()), u(rc.snapshots.load()),
              recovery.state().store().capacity());
  if (rs.log_errors || ps.log_full_waits) {
    std::printf("warning     %llu log errors, %llu log-full waits\n", u(rs.log_errors), u(ps.log_full_waits));
  }
  std::printf("book        %zu tracks as of seq %llu\n", recovery.state().book().size(),
              u(recovery.state().last_seq()));
  std::printf("book digest %s\n", recovery.state().book().hex_digest().c_str());
  return 0;
}
