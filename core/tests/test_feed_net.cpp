// The feed over real sockets on 127.0.0.1: publisher -> UDP -> subscriber,
// retransmit server (UDP) and snapshot server (TCP) on their own thread.
// The subscriber's reconstructed book must equal the publisher's byte for byte.

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"
#include "adsb/feed/subscriber.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;
using namespace std::chrono_literals;
using protocol::TrackDelete;
using protocol::TrackUpdate;

const protocol::Session kSession = protocol::make_session("NETTEST001");

// Publisher-side stack: recovery server on its own thread.
struct Server {
  std::unique_ptr<MessageLog> log = make_message_log();
  std::unique_ptr<RecoveryServer> rec;
  std::atomic<bool> stop{false};
  std::thread th;

  explicit Server(std::size_t ring = 1u << 16, LossConfig reply_loss = {},
                  const protocol::Session& session = kSession) {
    RecoveryServerConfig cfg;
    cfg.recovery.session = session;
    cfg.recovery.ring_capacity = ring;
    cfg.bind_host = "127.0.0.1";
    cfg.reply_loss = reply_loss;
    rec = std::make_unique<RecoveryServer>(cfg, *log);
    std::string err;
    EXPECT_TRUE(rec->open(&err)) << err;
    th = std::thread([this] { rec->run(stop); });
  }
  void finish() {
    stop = true;
    if (th.joinable()) th.join();
  }
  ~Server() { finish(); }
  Endpoint rtx() const { return {"127.0.0.1", rec->retransmit_port()}; }
  Endpoint snap() const { return {"127.0.0.1", rec->snapshot_port()}; }
};

// Deterministic message stream over 300 aircraft.
struct Traffic {
  Rng rng{17};
  FeedBook truth;
  template <typename Pub>
  void publish_one(Pub& pub, std::uint64_t i) {
    const auto icao = static_cast<std::uint32_t>(0xA00000 + rng.below(300));
    if (rng.chance(0.05)) {
      TrackDelete d;
      d.icao = icao;
      d.reason = 1;
      d.event_ts_ns = i;
      pub.on_delete(d);
    } else {
      TrackUpdate u;
      u.icao = icao;
      u.event_ts_ns = i;
      u.alt_ft = static_cast<std::int32_t>(rng.below(45000));
      u.lat_e7 = static_cast<std::int32_t>(rng.below(900000000));
      pub.on_update(u);
    }
  }
};

// Captures what the publisher actually sent (so the test knows the bytes,
// including publish_ts_ns) and forwards to the real UDP sender.
template <typename Inner>
struct TeeOut {
  Inner* inner;
  FeedBook* sent_book;
  bool send(std::span<const std::uint8_t> d) noexcept {
    if (const auto pv = protocol::parse_packet(d)) {
      pv->for_each([&](std::uint64_t, std::span<const std::uint8_t> m) { sent_book->apply(m); });
    }
    return inner->send(d);
  }
};

SubscriberClientConfig client_cfg(const Server& s, Endpoint feed) {
  SubscriberClientConfig c;
  c.feed = std::move(feed);
  c.interface_host = "127.0.0.1";
  c.retransmit = s.rtx();
  c.snapshot = s.snap();
  c.core.retransmit_timeout_ns = 5'000'000;
  c.core.snapshot_retry_ns = 50'000'000;
  return c;
}

struct ClientThread {
  SubscriberClient client;
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};  // run() returned: the core may be read
  std::thread th;
  explicit ClientThread(const SubscriberClientConfig& c) : client(c) {
    std::string err;
    EXPECT_TRUE(client.open(&err)) << err;
  }
  void start() {
    th = std::thread([this] {
      client.run(stop, /*stop_at_end=*/true);
      done.store(true, std::memory_order_release);
    });
  }
  // Waits for the subscriber to finish; `tick` runs meanwhile (the publisher's
  // poll(), which repeats the end-of-session marker). The core belongs to the
  // client thread until it is joined: polling client.core() from here was a
  // data race (GCC TSan caught it), so wait on `done`, which run() sets on
  // its way out once the session has ended.
  template <typename Tick = void (*)()>
  bool join_within(std::chrono::milliseconds t, Tick tick = [] {}) {
    const auto end = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < end && !done.load(std::memory_order_acquire)) {
      tick();
      std::this_thread::sleep_for(2ms);
    }
    stop = true;
    if (th.joinable()) th.join();
    return client.core().ended();
  }
  ~ClientThread() {
    stop = true;
    if (th.joinable()) th.join();
  }
};

// Publishes `n` messages through `pub`, pacing a little so the run takes
// tens of milliseconds, with an optional window in which the feed is cut.
template <typename Pub>
void run_publisher(Pub& pub, Traffic& t, std::uint64_t n) {
  for (std::uint64_t i = 1; i <= n; ++i) {
    t.publish_one(pub, i);
    if (i % 200 == 0) {
      pub.poll();
      std::this_thread::sleep_for(200us);
    }
  }
  pub.end_session(3);
}

TEST(FeedNet, SnapshotServerServesTheBookOverTcp) {
  Server s;
  for (std::uint64_t q = 1; q <= 500; ++q) {
    LogEntry e;
    e.seq = q;
    TrackUpdate u;
    u.icao = static_cast<std::uint32_t>(q % 97 + 1);
    u.alt_ft = static_cast<std::int32_t>(q);
    protocol::write(u, e.bytes);
    e.len = TrackUpdate::kWireSize;
    while (!s.log->try_push(e)) std::this_thread::yield();
  }
  SnapshotData snap;
  std::string err;
  // The server drains the log before building the snapshot; wait until it has all 500.
  for (int i = 0; i < 200 && s.rec->counters().logged.load() < 500; ++i) std::this_thread::sleep_for(2ms);
  ASSERT_TRUE(SubscriberClient::fetch_snapshot(s.snap(), 2000, snap, &err)) << err;
  EXPECT_EQ(snap.session, kSession);
  EXPECT_EQ(snap.as_of_seq, 500u);
  EXPECT_EQ(snap.tracks.size(), 97u);
  // Several clients at once.
  std::vector<std::thread> ths;
  std::atomic<int> ok{0};
  for (int i = 0; i < 8; ++i) {
    ths.emplace_back([&] {
      SnapshotData d;
      std::string e;
      if (SubscriberClient::fetch_snapshot(s.snap(), 2000, d, &e) && d.tracks.size() == 97) ++ok;
    });
  }
  for (auto& t : ths) t.join();
  EXPECT_EQ(ok.load(), 8);
  s.finish();
  EXPECT_EQ(s.rec->state().stats().snapshots, 9u);
}

TEST(FeedNet, SnapshotFetchFailsCleanlyWhenNothingListens) {
  std::uint16_t port;
  {
    const int fd = open_tcp_listener("127.0.0.1", 0, 1);
    port = local_port(fd);
    close_fd(fd);
  }
  SnapshotData d;
  std::string err;
  EXPECT_FALSE(SubscriberClient::fetch_snapshot({"127.0.0.1", port}, 300, d, &err));
  EXPECT_FALSE(err.empty());
}

void end_to_end(double feed_drop, double reply_drop, std::size_t ring, std::uint64_t n,
                std::uint64_t batch_ns, SubscriberCore::Mode* saw = nullptr) {
  LossConfig rl;
  rl.drop_rate = reply_drop;
  rl.seed = 5;
  Server s(ring, rl);
  ClientThread c(client_cfg(s, {"127.0.0.1", 0}));
  const Endpoint feed{"127.0.0.1", c.client.feed_port()};

  UdpSender udp;
  ASSERT_TRUE(udp.open(feed, UdpSender::Options{}));
  LossConfig fl;
  fl.drop_rate = feed_drop;
  fl.drop_burst = 3;
  fl.seed = 9;
  LossyOut<UdpSender> lossy(udp, fl);
  FeedBook sent;
  TeeOut<LossyOut<UdpSender>> tee{&lossy, &sent};
  PublisherConfig pc;
  pc.session = kSession;
  pc.max_batch_delay_ns = batch_ns;
  pc.heartbeat_interval_ns = 10'000'000;
  Publisher<TeeOut<LossyOut<UdpSender>>> pub(pc, tee, s.log.get());

  c.start();
  Traffic t;
  run_publisher(pub, t, n);
  const bool ended = c.join_within(20s, [&] { pub.poll(); });
  s.finish();

  const auto& core = c.client.core();
  EXPECT_TRUE(ended) << "expected " << core.expected() << " known " << core.known_next()
                     << " snapshot error '" << c.client.last_snapshot_error() << "'";
  EXPECT_EQ(core.expected(), pub.next_seq());
  EXPECT_TRUE(core.book() == s.rec->state().book()) << "subscriber book != publisher book";
  EXPECT_TRUE(core.book() == sent);
  EXPECT_EQ(core.book().hex_digest(), s.rec->state().book().hex_digest());
  if (feed_drop > 0) {
    EXPECT_GT(lossy.dropped(), 0u);
    EXPECT_GT(core.stats().requests, 0u);
  }
  if (saw) *saw = core.stats().snapshots_loaded > 0 ? SubscriberCore::Mode::kSnapshot : SubscriberCore::Mode::kLive;
}

TEST(FeedNet, EndToEndWithoutLoss) { end_to_end(0.0, 0.0, 1u << 16, 20'000, 0); }

TEST(FeedNet, EndToEndBatchedWithFeedAndReplyLoss) { end_to_end(0.03, 0.05, 1u << 16, 30'000, 20'000); }

TEST(FeedNet, EndToEndFallsBackToSnapshotsWhenTheRingIsSmall) {
  // A 64-message ring cannot cover gaps once the subscriber falls behind; it
  // must recover through TCP snapshots and still match byte for byte.
  SubscriberCore::Mode saw = SubscriberCore::Mode::kLive;
  end_to_end(0.08, 0.2, 64, 30'000, 0, &saw);
  EXPECT_EQ(saw, SubscriberCore::Mode::kSnapshot) << "the snapshot path was exercised";
}

TEST(FeedNet, LateJoinerRecoversBySnapshot) {
  Server s;
  ClientThread joiner(client_cfg(s, {"127.0.0.1", 0}));
  UdpSender udp;
  ASSERT_TRUE(udp.open({"127.0.0.1", joiner.client.feed_port()}, UdpSender::Options{}));
  // Until the subscriber "starts", datagrams go nowhere.
  struct GatedOut {
    UdpSender* udp;
    bool on = false;
    bool send(std::span<const std::uint8_t> d) noexcept { return on ? udp->send(d) : true; }
  } gate{&udp};
  PublisherConfig pc;
  pc.session = kSession;
  Publisher<GatedOut> pub(pc, gate, s.log.get());
  Traffic t;
  for (std::uint64_t i = 1; i <= 5000; ++i) t.publish_one(pub, i);
  gate.on = true;
  joiner.start();
  for (std::uint64_t i = 5001; i <= 10000; ++i) {
    t.publish_one(pub, i);
    if (i % 200 == 0) std::this_thread::sleep_for(200us);
  }
  pub.end_session(3);
  ASSERT_TRUE(joiner.join_within(10s, [&] { pub.poll(); }));
  s.finish();
  const auto& core = joiner.client.core();
  EXPECT_GE(core.stats().snapshots_loaded, 1u);
  EXPECT_LT(core.stats().applied, 6000u) << "joined by snapshot, not by replaying from 1";
  EXPECT_EQ(core.expected(), 10001u);
  EXPECT_TRUE(core.book() == s.rec->state().book());
}

// Multicast on the loopback interface. Some hosts (Linux without a multicast
// route on lo, sandboxed CI) cannot deliver it; the test then skips.
TEST(FeedNet, MulticastReachesTwoSubscribersOnOneHost) {
  Server s;
  const Endpoint group{"239.19.0.99", 41999};
  SubscriberClientConfig cfg = client_cfg(s, group);
  ClientThread a(cfg), b(cfg);
  UdpSender::Options opt;
  opt.interface_host = "127.0.0.1";
  UdpSender udp;
  std::string err;
  ASSERT_TRUE(udp.open(group, opt, &err)) << err;

  // Probe: does a heartbeat arrive at all?
  {
    protocol::PacketBuilder hb(protocol::make_session("PROBE00001"), 1);
    const auto d = hb.heartbeat();
    const int fd = open_feed_receiver(group, "127.0.0.1", 1 << 16);
    ASSERT_GE(fd, 0);
    bool got = false;
    for (int i = 0; i < 20 && !got; ++i) {
      udp.send(d);
      std::this_thread::sleep_for(10ms);
      std::uint8_t buf[64];
      got = ::recv(fd, buf, sizeof buf, 0) > 0;
    }
    close_fd(fd);
    if (!got) GTEST_SKIP() << "multicast loopback is not available on this host";
  }

  PublisherConfig pc;
  pc.session = kSession;
  pc.max_batch_delay_ns = 10'000;
  Publisher<UdpSender> pub(pc, udp, s.log.get());
  a.start();
  b.start();
  Traffic t;
  run_publisher(pub, t, 10'000);
  ASSERT_TRUE(a.join_within(10s, [&] { pub.poll(); }));
  ASSERT_TRUE(b.join_within(10s, [&] { pub.poll(); }));
  s.finish();
  EXPECT_TRUE(a.client.core().book() == s.rec->state().book());
  EXPECT_TRUE(b.client.core().book() == s.rec->state().book());
  // The probe session's heartbeats may have reached them first: they then
  // switched to the real session, which is fine.
  EXPECT_EQ(a.client.core().session(), kSession);
}

}  // namespace
