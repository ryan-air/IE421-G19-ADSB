// Shared-memory broadcast ring (shm_ring.hpp) and its subscriber
// (shm_subscriber.hpp): the slot protocol on ordinary memory (so
// ThreadSanitizer sees writer and readers on one region), torn-read and lap
// detection under concurrency, the POSIX shm object across processes, the
// publisher transport, and the subscriber's join / lap / replace recovery
// against the real snapshot server.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"
#include "adsb/feed/shm_ring.hpp"
#include "adsb/feed/shm_subscriber.hpp"
#include "adsb/feed/snapshot.hpp"

#include <poll.h>
#include <sys/socket.h>

namespace {

using namespace adsb;
using namespace adsb::feed;
using namespace std::chrono_literals;
using Read = RingView::Read;

const protocol::Session kS1 = protocol::make_session("SHMTEST001");
const protocol::Session kS2 = protocol::make_session("SHMTEST002");

// A ring on heap memory (header + slots), aligned like a mapping.
struct HeapRing {
  std::uint64_t cap;
  void* mem;
  RingView view;
  explicit HeapRing(std::uint64_t capacity, const protocol::Session& s = kS1, std::uint64_t first = 1)
      : cap(capacity),
        // aligned_alloc needs a size that is a multiple of the alignment.
        mem(std::aligned_alloc(4096, (shm_region_bytes(capacity) + 4095) / 4096 * 4096)),
        view(mem, capacity) {
    view.format(s, first, 42, 1);
  }
  ~HeapRing() { std::free(mem); }
};

// Message `seq`: a full slot (72 bytes), every 8-byte word derived from seq, so a torn copy
// (words from two different messages) is detectable.
std::vector<std::uint8_t> pattern(std::uint64_t seq) {
  std::vector<std::uint8_t> m(kShmMaxMessage);
  for (std::uint64_t i = 0; i < kShmMaxMessage / 8; ++i) {
    const std::uint64_t w = seq * 0x9E3779B97F4A7C15ULL ^ (i << 56);
    std::memcpy(m.data() + 8 * i, &w, 8);
  }
  return m;
}
bool matches(std::uint64_t seq, const std::uint8_t* m) {
  const auto p = pattern(seq);
  return std::memcmp(p.data(), m, kShmMaxMessage) == 0;
}

std::string unique_name(const char* tag) {
  static int n = 0;
  return "/adsbt-" + std::string(tag) + "-" + std::to_string(::getpid() % 100000) + "-" + std::to_string(n++);
}

TEST(ShmRing, WritesAreReadInOrderAndEmptyUntilWritten) {
  HeapRing r(8);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  EXPECT_EQ(r.view.read(1, buf, len), Read::kEmpty);
  for (std::uint64_t s = 1; s <= 5; ++s) r.view.write(s, pattern(s).data(), kShmMaxMessage);
  for (std::uint64_t s = 1; s <= 5; ++s) {
    ASSERT_EQ(r.view.read(s, buf, len), Read::kOk) << s;
    EXPECT_EQ(len, kShmMaxMessage);
    EXPECT_TRUE(matches(s, buf));
  }
  EXPECT_EQ(r.view.read(6, buf, len), Read::kEmpty);
  EXPECT_EQ(r.view.next_seq(), 6u);
  EXPECT_EQ(r.view.oldest(), 1u);
  EXPECT_EQ(r.view.session(), kS1);
}

TEST(ShmRing, ShortMessagesKeepTheirLength) {
  HeapRing r(4);
  const std::uint8_t del[16] = {'D', 0, 1, 0, 7, 0, 0, 0};
  r.view.write(1, del, sizeof del);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  ASSERT_EQ(r.view.read(1, buf, len), Read::kOk);
  EXPECT_EQ(len, 16u);
  EXPECT_EQ(std::memcmp(buf, del, 16), 0);
}

TEST(ShmRing, ALappedReaderIsToldSoNeverGivenNewerData) {
  HeapRing r(8);
  for (std::uint64_t s = 1; s <= 20; ++s) r.view.write(s, pattern(s).data(), kShmMaxMessage);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  EXPECT_EQ(r.view.read(3, buf, len), Read::kLapped) << "slot 3 now holds 19";
  EXPECT_EQ(r.view.read(12, buf, len), Read::kLapped) << "an 8-slot ring holds 13..20";
  EXPECT_EQ(r.view.read(13, buf, len), Read::kOk);
  EXPECT_TRUE(matches(13, buf));
  EXPECT_EQ(r.view.oldest(), 13u);
  EXPECT_EQ(r.view.read(21, buf, len), Read::kEmpty);
}

TEST(ShmRing, ASlotBeingWrittenReadsAsEmptyAndALaterBusyOneAsLapped) {
  HeapRing r(4);
  r.view.write(1, pattern(1).data(), kShmMaxMessage);
  // Simulate the writer midway through message 2, then midway through 6 (slot 2 again).
  auto* slot = reinterpret_cast<ShmSlot*>(static_cast<std::uint8_t*>(r.mem) + kShmHeaderBytes);
  slot[2].seq.store(kShmBusy | 2, std::memory_order_relaxed);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  EXPECT_EQ(r.view.read(2, buf, len), Read::kEmpty);
  slot[2].seq.store(kShmBusy | 6, std::memory_order_relaxed);
  EXPECT_EQ(r.view.read(2, buf, len), Read::kLapped);
  // The lap was seen through the busy marker, but next_seq is still 2 (on
  // weak memory a reader can see exactly this; the GenMC model found it).
  // Resuming at next_seq() would retry the lost message forever-ish; the
  // helper moves forward.
  EXPECT_EQ(r.view.next_seq(), 2u);
  EXPECT_EQ(r.view.resume_after_lap(2), 3u);
  for (std::uint64_t s = 2; s <= 9; ++s) r.view.write(s, pattern(s).data(), kShmMaxMessage);
  EXPECT_EQ(r.view.resume_after_lap(2), 10u) << "normally the head";
}

TEST(ShmRing, EndOfSessionIsVisible) {
  HeapRing r(4);
  EXPECT_EQ(r.view.ended(), 0u);
  r.view.write(1, pattern(1).data(), kShmMaxMessage);
  r.view.end(2);
  EXPECT_EQ(r.view.ended(), 2u);
}

TEST(ShmRing, NoTornReadsUnderConcurrencyAndLapsAreDetected) {
  // A small ring and a writer at full speed: readers get lapped constantly.
  // Every message a reader accepts must be intact and in order; every lap must
  // be reported, never papered over.
  HeapRing r(64);
  constexpr std::uint64_t kMessages = 400'000;
  std::atomic<bool> go{false};
  struct Counts {
    std::uint64_t ok = 0, laps = 0, torn = 0, out_of_order = 0;
  };
  std::vector<Counts> counts(3);
  std::vector<std::thread> readers;
  for (std::size_t t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      while (!go.load()) {}
      std::uint64_t expect = 1, last = 0;
      std::uint8_t buf[kShmMaxMessage];
      std::size_t len = 0;
      while (expect <= kMessages) {
        const Read res = r.view.read(expect, buf, len);
        if (res == Read::kOk) {
          if (!matches(expect, buf)) ++counts[t].torn;
          if (expect <= last) ++counts[t].out_of_order;
          last = expect;
          ++counts[t].ok;
          ++expect;
          // A slow reader: a 1 ms pause every 1,000 messages is laps of a
          // 64-slot ring at any writer speed (including under TSan).
          if (t == 2 && counts[t].ok % 1000 == 0) std::this_thread::sleep_for(1ms);
        } else if (res == Read::kLapped) {
          ++counts[t].laps;
          expect = r.view.resume_after_lap(expect);  // resync at the head
          if (expect > kMessages) break;
        } else if (r.view.next_seq() > kMessages && expect >= r.view.next_seq()) {
          break;
        }
      }
    });
  }
  go = true;
  for (std::uint64_t s = 1; s <= kMessages; ++s) r.view.write(s, pattern(s).data(), kShmMaxMessage);
  for (auto& th : readers) th.join();
  std::uint64_t ok = 0, laps = 0;
  for (const auto& c : counts) {
    EXPECT_EQ(c.torn, 0u);
    EXPECT_EQ(c.out_of_order, 0u);
    ok += c.ok;
    laps += c.laps;
  }
  EXPECT_GT(ok, 1000u);
  EXPECT_GT(counts[2].laps, 0u) << "the slow reader must have been lapped (non-vacuity)";
  (void)laps;
}

TEST(ShmRegion, CreateOpenAndValidate) {
  const std::string name = unique_name("open");
  std::string err;
  ShmRegion w;
  ASSERT_TRUE(w.create(name, 16, kS1, 1, &err)) << err;
  w.view().write(1, pattern(1).data(), kShmMaxMessage);
  ShmRegion rd;
  ASSERT_TRUE(rd.open(name, &err)) << err;
  EXPECT_EQ(rd.view().capacity(), 16u);
  EXPECT_EQ(rd.view().session(), kS1);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  ASSERT_EQ(rd.view().read(1, buf, len), Read::kOk);
  EXPECT_TRUE(matches(1, buf));
  EXPECT_EQ(ShmRegion::current_instance(name), w.view().instance());

  ShmRegion bad;
  EXPECT_FALSE(bad.open(unique_name("none"), &err));
  EXPECT_FALSE(bad.open("no-slash", &err));
  EXPECT_FALSE(w.create(unique_name("cap"), 3, kS1, 1, &err)) << "capacity must be a power of two";
}

TEST(ShmRegion, ReplacementGetsANewInstanceAndOldReadersKeepTheirMapping) {
  const std::string name = unique_name("repl");
  std::string err;
  auto w1 = std::make_unique<ShmRegion>();
  ASSERT_TRUE(w1->create(name, 8, kS1, 1, &err)) << err;
  w1->view().write(1, pattern(1).data(), kShmMaxMessage);
  w1->view().end(2);
  ShmRegion rd;
  ASSERT_TRUE(rd.open(name, &err)) << err;
  const std::uint64_t old_id = rd.view().instance();
  ShmRegion w2;
  ASSERT_TRUE(w2.create(name, 8, kS2, 1, &err)) << err;  // a restarted publisher
  EXPECT_NE(ShmRegion::current_instance(name), old_id);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  EXPECT_EQ(rd.view().read(1, buf, len), Read::kOk) << "the old mapping stays readable";
  EXPECT_EQ(rd.view().ended(), 2u);
  w1.reset();  // the old writer's destructor must not unlink the new ring
  EXPECT_NE(ShmRegion::current_instance(name), 0u);
}

TEST(ShmRegion, AnotherProcessReadsWhatThisOneWrites) {
  const std::string name = unique_name("fork");
  std::string err;
  ShmRegion w;
  ASSERT_TRUE(w.create(name, 1024, kS1, 1, &err)) << err;
  constexpr std::uint64_t kN = 50'000;
  int ready[2];
  ASSERT_EQ(::pipe(ready), 0);
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {  // child: read every message it can (laps are allowed, torn reads are not)
    ShmRegion rd;
    std::string e;
    if (!rd.open(name, &e)) ::_exit(10);
    const char one = 1;
    if (::write(ready[1], &one, 1) != 1) ::_exit(13);
    std::uint64_t expect = 1, ok = 0;
    std::uint8_t buf[kShmMaxMessage];
    std::size_t len = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (expect <= kN && std::chrono::steady_clock::now() < deadline) {
      const Read res = rd.view().read(expect, buf, len);
      if (res == Read::kOk) {
        if (!matches(expect, buf)) ::_exit(11);
        ++ok;
        ++expect;
      } else if (res == Read::kLapped) {
        expect = rd.view().resume_after_lap(expect);
      }
    }
    ::_exit(ok > kN / 10 ? 0 : 12);
  }
  // Wait until the child has mapped the ring, then write at a pace (about
  // 50 ms in all) that a briefly descheduled child can follow: a fixed 50 ms
  // head start was not enough under parallel test load.
  char c = 0;
  ASSERT_EQ(::read(ready[0], &c, 1), 1);
  ::close(ready[0]);
  ::close(ready[1]);
  for (std::uint64_t s = 1; s <= kN; ++s) {
    w.view().write(s, pattern(s).data(), kShmMaxMessage);
    if (s % 1024 == 0) std::this_thread::sleep_for(1ms);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "10: open failed, 11: torn message, 12: too few read, 13: pipe";
}

// ---------------------------------------------------------------------------
// Publisher transport and subscriber
// ---------------------------------------------------------------------------

struct Server {
  std::unique_ptr<MessageLog> log = make_message_log();
  std::unique_ptr<RecoveryServer> rec;
  std::atomic<bool> stop{false};
  std::thread th;
  explicit Server(const protocol::Session& s, std::uint16_t snapshot_port = 0) {
    RecoveryServerConfig cfg;
    cfg.recovery.session = s;
    cfg.bind_host = "127.0.0.1";
    cfg.retransmit_port = 0;
    cfg.snapshot_port = snapshot_port;
    rec = std::make_unique<RecoveryServer>(cfg, *log);
    std::string err;
    EXPECT_TRUE(rec->open(&err)) << err;
    th = std::thread([this] { rec->run(stop); });
  }
  ~Server() {
    stop = true;
    if (th.joinable()) th.join();
  }
  Endpoint snap() const { return {"127.0.0.1", rec->snapshot_port()}; }
};

// Records the book of everything the publisher sent (on the publisher's own
// thread, so the test never reads the recovery thread's state concurrently).
struct CaptureOut {
  FeedBook book;
  bool send(std::span<const std::uint8_t> d) noexcept {
    if (const auto pv = protocol::parse_packet(d)) {
      pv->for_each([&](std::uint64_t, std::span<const std::uint8_t> m) { book.apply(m); });
    }
    return true;
  }
};
using TestOut = DualOut<ShmOut, CaptureOut>;

// Publishes deterministic traffic over 200 aircraft to the ring.
struct ShmPub {
  ShmRegion region;
  std::unique_ptr<ShmOut> out;
  CaptureOut capture;
  std::unique_ptr<TestOut> dual;
  std::unique_ptr<Server> server;
  std::unique_ptr<Publisher<TestOut>> pub;
  Rng rng{9};
  std::uint64_t n = 0;
  ShmPub(const std::string& name, std::uint64_t cap, const protocol::Session& s, std::uint64_t batch_ns = 0,
         std::uint16_t snapshot_port = 0) {
    std::string err;
    EXPECT_TRUE(region.create(name, cap, s, 1, &err)) << err;
    out = std::make_unique<ShmOut>(region.view());
    dual = std::make_unique<TestOut>(out.get(), &capture);
    server = std::make_unique<Server>(s, snapshot_port);
    PublisherConfig pc;
    pc.session = s;
    pc.max_batch_delay_ns = batch_ns;
    pub = std::make_unique<Publisher<TestOut>>(pc, *dual, server->log.get());
  }
  void publish(std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i, ++n) {
      const auto icao = static_cast<std::uint32_t>(0xC00000 + rng.below(200));
      if (rng.chance(0.05)) {
        protocol::TrackDelete d;
        d.icao = icao;
        pub->on_delete(d);
      } else {
        protocol::TrackUpdate u;
        u.icao = icao;
        u.alt_ft = static_cast<std::int32_t>(n);
        pub->on_update(u);
      }
    }
  }
  // The book of everything published so far.
  std::string digest() const { return capture.book.hex_digest(); }
};

void poll_until(ShmSubscriber& sub, std::uint64_t until_seq, std::chrono::milliseconds limit = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end && !(sub.state() == ShmSubscriber::State::kLive && sub.expected() > until_seq)) {
    sub.poll(mono_ns());
  }
}

TEST(ShmOut, PublisherMessagesAppearWithTheirSequenceNumbers) {
  const std::string name = unique_name("pub");
  ShmPub p(name, 1024, kS1);
  p.publish(100);
  p.pub->end_session(1);
  RingView v = p.region.view();
  EXPECT_EQ(v.next_seq(), 101u);
  EXPECT_EQ(v.ended(), 101u);
  std::uint8_t buf[kShmMaxMessage];
  std::size_t len = 0;
  FeedBook book;
  for (std::uint64_t s = 1; s <= 100; ++s) {
    ASSERT_EQ(v.read(s, buf, len), Read::kOk);
    book.apply(std::span<const std::uint8_t>(buf, len));
  }
  EXPECT_EQ(book.hex_digest(), p.digest());
}

TEST(ShmSubscriber, JoinsFromTheStartWithoutASnapshotAndFollows) {
  const std::string name = unique_name("join");
  ShmPub p(name, 4096, kS1);
  p.publish(500);
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = p.server->snap();
  ShmSubscriber sub(cfg);
  std::string err;
  ASSERT_TRUE(sub.open(&err)) << err;
  poll_until(sub, 500);
  p.publish(700);
  p.pub->end_session(1);
  poll_until(sub, 1200);
  EXPECT_TRUE(sub.ended());
  EXPECT_EQ(sub.stats().snapshots_loaded, 0u);
  EXPECT_EQ(sub.stats().laps, 0u);
  EXPECT_EQ(sub.book().hex_digest(), p.digest());
}

TEST(ShmSubscriber, ALateJoinerUsesASnapshot) {
  const std::string name = unique_name("late");
  ShmPub p(name, 256, kS1);
  p.publish(5000);  // the ring no longer holds message 1
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = p.server->snap();
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  poll_until(sub, 4999);
  p.publish(100);
  poll_until(sub, 5100);
  EXPECT_EQ(sub.stats().snapshots_loaded, 1u);
  EXPECT_EQ(sub.stats().laps, 0u) << "a late joiner goes straight to a snapshot, it is not lapped";
  EXPECT_EQ(sub.book().hex_digest(), p.digest());
}

TEST(ShmSubscriber, ALappedSubscriberResyncsFromASnapshot) {
  const std::string name = unique_name("lap");
  ShmPub p(name, 128, kS1);
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = p.server->snap();
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  p.publish(50);
  poll_until(sub, 50);
  p.publish(2000);  // far more than the ring holds, while the subscriber is not reading
  poll_until(sub, 2050);
  EXPECT_GE(sub.stats().laps, 1u);
  EXPECT_GE(sub.stats().snapshots_loaded, 1u);
  EXPECT_EQ(sub.book().hex_digest(), p.digest());
}

TEST(ShmSubscriber, ARestartedPublisherReplacesTheRingAndTheSubscriberFollows) {
  const std::string name = unique_name("rst");
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.replace_check_ns = 0;
  auto p1 = std::make_unique<ShmPub>(name, 1024, kS1);
  cfg.snapshot = p1->server->snap();
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  p1->publish(300);
  p1->pub->end_session(1);
  poll_until(sub, 300);
  EXPECT_TRUE(sub.ended());
  ShmPub p2(name, 1024, kS2);  // restart (the old ring is replaced)
  p2.publish(100);
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < end && !(sub.session() == kS2 && sub.expected() > 100)) sub.poll(mono_ns());
  EXPECT_EQ(sub.session(), kS2);
  EXPECT_EQ(sub.stats().rings_opened, 2u);
  EXPECT_EQ(sub.book().hex_digest(), p2.digest()) << "nothing from the old session survives";
  p1.reset();
}

TEST(ShmSubscriber, WaitsForARingThatDoesNotExistYet) {
  const std::string name = unique_name("wait");
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.reopen_retry_ns = 0;
  ShmSubscriber sub(cfg);
  EXPECT_FALSE(sub.open());
  EXPECT_EQ(sub.poll(mono_ns()), 0u);
  ShmPub p(name, 1024, kS1);  // the whole session is in the ring: it joins from the start
  p.publish(10);
  poll_until(sub, 10);
  EXPECT_EQ(sub.expected(), 11u);
  EXPECT_EQ(sub.stats().snapshots_loaded, 0u);
}

TEST(ShmRegion, RejectsFirstSeqZero) {
  ShmRegion w;
  std::string err;
  EXPECT_FALSE(w.create(unique_name("zero"), 8, kS1, 0, &err)) << "seq 0 marks a never-written slot";
}

TEST(ShmSubscriber, ASnapshotNewerThanTheRingIsUsedAndTheRestFollows) {
  // A batching publisher logs messages for recovery before it sends them, so
  // the snapshot can be ahead of the ring. The subscriber must wait for the
  // ring to catch up, not reject the snapshot or skip messages.
  const std::string name = unique_name("newer");
  ShmPub p(name, 256, kS1, /*batch_ns=*/60'000'000'000ULL);  // flush only when a datagram is full
  p.publish(5000 + 7);  // wraps the ring; the last 7 messages are pending in the batch
  std::this_thread::sleep_for(100ms);  // let the recovery thread take them into the snapshot book
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = p.server->snap();
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < end && sub.state() != ShmSubscriber::State::kLive) sub.poll(mono_ns());
  ASSERT_EQ(sub.state(), ShmSubscriber::State::kLive);
  EXPECT_GT(sub.expected(), p.region.view().next_seq()) << "the snapshot was newer than the ring (non-vacuity)";
  p.pub->flush();
  poll_until(sub, p.pub->next_seq() - 1);
  EXPECT_EQ(sub.book().hex_digest(), p.digest());
}

TEST(ShmSubscriber, ARestartWithTheSameSessionIdIsNotPairedWithTheOldRing) {
  // Review finding (reproduced before the fix): a restarted publisher that
  // reuses the session id serves snapshots of its own state on the same port.
  // A subscriber still holding the old ring must not combine that snapshot
  // with the old ring's messages; it must switch to the new ring.
  const std::string name = unique_name("same");
  auto p1 = std::make_unique<ShmPub>(name, 128, kS1);
  p1->publish(1000);  // wrapped (oldest 873): a subscriber needs a snapshot
  p1->pub->end_session(1);
  const std::uint16_t port = p1->server->rec->snapshot_port();
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = {"127.0.0.1", port};
  cfg.replace_check_ns = 60'000'000'000ULL;  // only the post-snapshot check can catch it
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());  // holds ring 1, not yet synced
  p1.reset();               // publisher 1 exits (its ring stays mapped by the subscriber)
  ShmPub p2(name, 128, kS1, 0, port);  // same session id, same snapshot port, new ring
  ASSERT_EQ(p2.server->rec->snapshot_port(), port);
  p2.publish(950);  // as_of 950 lies inside ring 1's range (873..1000): the dangerous case
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < end && !(sub.state() == ShmSubscriber::State::kLive && sub.expected() > 950)) {
    sub.poll(mono_ns());
  }
  EXPECT_FALSE(sub.ended()) << "ring 1's end must not be taken for this session's";
  EXPECT_EQ(sub.stats().rings_opened, 2u) << "it switched to the new ring";
  EXPECT_EQ(sub.book().hex_digest(), p2.digest());
}

// Serves one fixed snapshot to every connection: lets a test hand the
// subscriber a snapshot the real server would never produce (stale, or from
// another session).
struct FakeSnapshotServer {
  int fd = -1;
  std::vector<std::uint8_t> bytes;
  std::atomic<bool> stop{false};
  std::thread th;
  FakeSnapshotServer(const protocol::Session& s, std::uint64_t as_of, const FeedBook& book) {
    serialize_snapshot(s, as_of, 0, book, bytes);
    std::string err;
    fd = open_tcp_listener("127.0.0.1", 0, 8, &err);
    EXPECT_GE(fd, 0) << err;
    th = std::thread([this] {
      while (!stop.load()) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 20) <= 0) continue;
        const int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        std::size_t off = 0;
        while (off < bytes.size()) {
          const ssize_t w = ::send(c, bytes.data() + off, bytes.size() - off, 0);
          if (w <= 0) break;
          off += static_cast<std::size_t>(w);
        }
        ::close(c);
      }
    });
  }
  ~FakeSnapshotServer() {
    stop = true;
    th.join();
    close_fd(fd);
  }
  Endpoint ep() const { return {"127.0.0.1", local_port(fd)}; }
};

// Polls for a while; returns true if the subscriber ever went live.
bool goes_live(ShmSubscriber& sub, std::chrono::milliseconds for_how_long) {
  const auto end = std::chrono::steady_clock::now() + for_how_long;
  while (std::chrono::steady_clock::now() < end) {
    sub.poll(mono_ns());
    if (sub.state() == ShmSubscriber::State::kLive) return true;
  }
  return false;
}

TEST(ShmSubscriber, RejectsASnapshotOlderThanTheRing) {
  // The ring holds 4873..5000; a snapshot as of 100 would leave a gap.
  const std::string name = unique_name("stale");
  ShmPub p(name, 128, kS1);
  p.publish(5000);
  FakeSnapshotServer fake(kS1, 100, FeedBook{});
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = fake.ep();
  cfg.snapshot_retry_ns = 1'000'000;
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  EXPECT_FALSE(goes_live(sub, 300ms));
  EXPECT_GT(sub.stats().snapshot_failures, 0u);
  EXPECT_EQ(sub.stats().applied, 0u) << "nothing applied across the gap";
}

TEST(ShmSubscriber, RejectsASnapshotFromAnotherSession) {
  const std::string name = unique_name("othersess");
  ShmPub p(name, 128, kS1);
  p.publish(1000);  // ring holds 873..1000
  FeedBook other;
  other.put([] {
    protocol::TrackUpdate u;
    u.icao = 0xABCDEF;
    return u;
  }());
  FakeSnapshotServer fake(kS2, 950, other);  // in range, but session S2
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = fake.ep();
  cfg.snapshot_retry_ns = 1'000'000;
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  EXPECT_FALSE(goes_live(sub, 300ms));
  EXPECT_GT(sub.stats().snapshot_failures, 0u);
  EXPECT_EQ(sub.book().find(0xABCDEF), nullptr) << "the other session's state was not adopted";
}

TEST(ShmSubscriber, GivesUpWhenLappedAfterTheEndWithNoSnapshotServer) {
  const std::string name = unique_name("giveup");
  auto p = std::make_unique<ShmPub>(name, 64, kS1);
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = p->server->snap();
  cfg.give_up_after_end_ns = 200'000'000;
  cfg.snapshot_timeout_ms = 100;
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  p->publish(10);
  poll_until(sub, 10);
  p->publish(1000);  // laps the subscriber
  p->pub->end_session(1);
  p->server.reset();  // the publisher is gone: no snapshots
  std::atomic<bool> stop{false};
  std::thread guard([&] {
    std::this_thread::sleep_for(5s);
    stop = true;
  });
  sub.run(stop, /*stop_at_end=*/true);
  stop = true;
  guard.join();
  EXPECT_TRUE(sub.gave_up()) << "must report, not wait forever";
  EXPECT_FALSE(sub.ended());
  EXPECT_GE(sub.stats().laps, 1u);
}

TEST(ShmSubscriber, AFailedReopenKeepsTheRingItHas) {
  const std::string name = unique_name("keep");
  ShmPub p(name, 1024, kS1);
  ShmSubscriberConfig cfg;
  cfg.name = name;
  cfg.snapshot = p.server->snap();
  ShmSubscriber sub(cfg);
  ASSERT_TRUE(sub.open());
  p.publish(20);
  poll_until(sub, 20);
  ShmRegion::unlink(name);  // the name disappears (e.g. replaced, then removed)
  std::string err;
  EXPECT_FALSE(sub.open(&err));
  EXPECT_NE(sub.state(), ShmSubscriber::State::kNoRing) << "the mapped ring is kept";
  p.publish(5);  // still written to the (unnamed) ring we map
  poll_until(sub, 25);
  EXPECT_EQ(sub.expected(), 26u);
}

}  // namespace
