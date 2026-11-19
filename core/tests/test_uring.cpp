// io_uring feed transport (feed/uring_sender.hpp, feed/tx.hpp). Skipped where
// the build has no io_uring (macOS) or the kernel refuses it (old kernel,
// a container's seccomp profile).

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "adsb/feed/net.hpp"
#include "adsb/feed/tx.hpp"
#include "adsb/feed/uring_sender.hpp"

namespace {

using namespace adsb::feed;
using namespace std::chrono_literals;

// Reads on its own thread while the test sends, so a small kernel receive
// buffer (rmem_max caps SO_RCVBUF, about 200 KB in containers) does not drop
// datagrams. The thread owns `got_`; the test reads it only after join().
class Receiver {
 public:
  explicit Receiver(int rcvbuf = 8 << 20) {
    fd_ = open_udp("127.0.0.1", 0, rcvbuf);
    port = local_port(fd_);
    start();
  }
  ~Receiver() {
    stop_and_join();
    close_fd(fd_);
  }
  // Waits until nothing has arrived for `quiet`, then returns what arrived
  // since the last drain (and keeps receiving).
  std::vector<std::vector<std::uint8_t>> drain(std::chrono::milliseconds quiet = 200ms) {
    std::uint64_t seen = count_.load();
    auto last = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - last < quiet) {
      std::this_thread::sleep_for(5ms);
      if (const std::uint64_t c = count_.load(); c != seen) {
        seen = c;
        last = std::chrono::steady_clock::now();
      }
    }
    stop_and_join();
    std::vector<std::vector<std::uint8_t>> out = std::move(got_);
    got_.clear();
    start();
    return out;
  }
  // Waits until `n` datagrams have arrived in total (or `timeout`). The tests
  // send in windows and wait, so a host stall cannot overflow the kernel
  // receive buffer: these tests are about correctness, not throughput.
  bool wait_count(std::uint64_t n, std::chrono::milliseconds timeout = 3s) const {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (count_.load() < n) {
      if (std::chrono::steady_clock::now() > end) return false;
      std::this_thread::yield();
    }
    return true;
  }
  std::uint16_t port = 0;

 private:
  void start() {
    stop_ = false;
    thread_ = std::thread([this] {
      std::uint8_t buf[4096];
      while (!stop_.load(std::memory_order_relaxed)) {
        const ssize_t n = ::recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
          got_.emplace_back(buf, buf + n);
          count_.fetch_add(1);
        }
      }
    });
  }
  void stop_and_join() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> count_{0};
  std::vector<std::vector<std::uint8_t>> got_;
  std::thread thread_;
};

std::vector<std::uint8_t> datagram(std::uint64_t seq, std::size_t len) {
  std::vector<std::uint8_t> d(len);
  for (std::size_t i = 0; i < len; ++i) d[i] = static_cast<std::uint8_t>(seq * 31 + i);
  std::memcpy(d.data(), &seq, 8);
  return d;
}

// Opens or skips the test when io_uring is not available here.
#define OPEN_OR_SKIP(sender, dest, opt)                                              \
  do {                                                                               \
    if (!UringSender::built()) GTEST_SKIP() << "built without io_uring";            \
    std::string err_;                                                                \
    if (!(sender).open((dest), (opt), &err_)) GTEST_SKIP() << "io_uring unavailable: " << err_; \
  } while (0)

class UringSend : public ::testing::TestWithParam<bool> {};

TEST_P(UringSend, EveryDatagramArrivesIntactAndInOrder) {
  Receiver rx;
  UringSender tx;
  UringSender::Options opt;
  opt.sqpoll = GetParam();
  opt.max_in_flight = 16;
  OPEN_OR_SKIP(tx, (Endpoint{"127.0.0.1", rx.port}), opt);
  constexpr std::uint64_t kN = 2000;
  for (std::uint64_t s = 1; s <= kN; ++s) {
    const auto d = datagram(s, 64 + s % 1200);
    ASSERT_TRUE(tx.send(d));
    if (s % 64 == 0) {
      ASSERT_TRUE(rx.wait_count(s)) << "lost before " << s;
    }
  }
  ASSERT_TRUE(tx.drain());
  const auto got = rx.drain();
  ASSERT_EQ(got.size(), kN);
  for (std::uint64_t s = 1; s <= kN; ++s) {
    EXPECT_EQ(got[s - 1], datagram(s, 64 + s % 1200)) << s;
  }
  const auto& st = tx.stats();
  EXPECT_EQ(st.queued, kN);
  EXPECT_EQ(st.completed, kN);
  EXPECT_EQ(st.failed, 0u);
  EXPECT_EQ(st.reordered, 0u);
  if (GetParam()) {
    EXPECT_EQ(st.submit_calls, 0u) << "SQPOLL: no io_uring_enter per datagram";
  } else {
    EXPECT_EQ(st.submit_calls, kN);
  }
}

INSTANTIATE_TEST_SUITE_P(SqpollAndNot, UringSend, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& p) { return p.param ? "Sqpoll" : "NoSqpoll"; });

TEST(UringSender, TwoBuffersStillDeliverEverythingInOrder) {
  Receiver rx;
  UringSender tx;
  UringSender::Options opt;
  opt.max_in_flight = 2;  // send() constantly waits for a buffer
  OPEN_OR_SKIP(tx, (Endpoint{"127.0.0.1", rx.port}), opt);
  constexpr std::uint64_t kN = 5000;
  for (std::uint64_t s = 1; s <= kN; ++s) {
    ASSERT_TRUE(tx.send(datagram(s, 100)));
    if (s % 64 == 0) {
      ASSERT_TRUE(rx.wait_count(s)) << "lost before " << s;
    }
  }
  ASSERT_TRUE(tx.drain());
  const auto got = rx.drain();
  ASSERT_EQ(got.size(), kN);
  for (std::uint64_t s = 1; s <= kN; ++s) {
    std::uint64_t seq;
    std::memcpy(&seq, got[s - 1].data(), 8);
    ASSERT_EQ(seq, s);
  }
  EXPECT_EQ(tx.stats().reordered, 0u);
  EXPECT_EQ(tx.stats().failed, 0u);
}

// A full send buffer must lose datagrams (counted, with EAGAIN; retransmit
// recovers them), never reorder them. Loopback UDP never fills the send
// buffer (it frees each datagram as it delivers it), so this uses a Unix
// datagram socket pair, whose queue (about 10 datagrams) pushes back on the
// sender while a slow reader drains it.
TEST(UringSender, AFullSendBufferLosesDatagramsButNeverReorders) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);
  UringSender tx;
  UringSender::Options opt;
  opt.max_in_flight = 256;
  if (!UringSender::built()) {
    close_fd(sv[0]);
    close_fd(sv[1]);
    GTEST_SKIP() << "built without io_uring";
  }
  std::string err;
  if (!tx.open_connected(sv[0], opt, &err)) {
    close_fd(sv[1]);
    GTEST_SKIP() << "io_uring unavailable: " << err;
  }
  std::atomic<bool> done{false};
  std::vector<std::uint64_t> seqs;  // owned by the reader until join()
  std::thread reader([&] {
    std::uint8_t buf[2048];
    std::uint64_t n = 0;
    for (;;) {
      const ssize_t r = ::recv(sv[1], buf, sizeof buf, MSG_DONTWAIT);
      if (r >= 8) {
        std::uint64_t seq;
        std::memcpy(&seq, buf, 8);
        seqs.push_back(seq);
        if (++n % 16 == 0) std::this_thread::sleep_for(50us);  // slower than the sender
      } else if (done.load()) {
        break;
      }
    }
  });
  constexpr std::uint64_t kN = 20000;
  for (std::uint64_t s = 1; s <= kN; ++s) ASSERT_TRUE(tx.send(datagram(s, 200)));
  ASSERT_TRUE(tx.drain(5000));
  std::this_thread::sleep_for(100ms);  // let the reader empty the queue
  done = true;
  reader.join();
  close_fd(sv[1]);

  const auto& st = tx.stats();
  EXPECT_EQ(st.completed, kN);
  EXPECT_EQ(st.reordered, 0u);
  EXPECT_GT(st.failed, 0u) << "non-vacuity: the send buffer was full at some point";
  EXPECT_EQ(st.last_error, EAGAIN);
  EXPECT_EQ(seqs.size() + st.failed, kN) << "every datagram is delivered or counted as lost";
  for (std::size_t i = 1; i < seqs.size(); ++i) ASSERT_GT(seqs[i], seqs[i - 1]) << "reordered at " << i;
  RecordProperty("sender_failed", std::to_string(st.failed));
}

TEST(UringSender, RejectsWhatItCannotSend) {
  Receiver rx;
  UringSender tx;
  EXPECT_FALSE(tx.send(datagram(1, 10))) << "not open";
  UringSender::Options bad;
  bad.max_in_flight = 0;
  std::string err;
  EXPECT_FALSE(tx.open(Endpoint{"127.0.0.1", rx.port}, bad, &err));
  UringSender::Options opt;
  OPEN_OR_SKIP(tx, (Endpoint{"127.0.0.1", rx.port}), opt);
  EXPECT_FALSE(tx.send(std::vector<std::uint8_t>(UringSender::kBufferBytes + 1)));
  EXPECT_FALSE(tx.send({}));
  EXPECT_TRUE(tx.send(datagram(1, UringSender::kBufferBytes)));
  EXPECT_TRUE(tx.drain());
  EXPECT_EQ(rx.drain().size(), 1u);
}

TEST(FeedTx, ParsesTransportsAndSendsThroughEither) {
  Transport t{};
  EXPECT_TRUE(parse_transport("kernel", t));
  EXPECT_EQ(t, Transport::kKernel);
  EXPECT_TRUE(parse_transport("uring", t));
  EXPECT_EQ(t, Transport::kUring);
  EXPECT_TRUE(parse_transport("uring-nosqpoll", t));
  EXPECT_EQ(t, Transport::kUringNoSqpoll);
  EXPECT_FALSE(parse_transport("xdp", t));

  Receiver rx;
  FeedTx k;
  ASSERT_TRUE(k.open(Endpoint{"127.0.0.1", rx.port}, Transport::kKernel, {}, {}));
  EXPECT_TRUE(k.send(datagram(7, 50)));
  EXPECT_TRUE(k.report().empty());
  ASSERT_EQ(rx.drain().size(), 1u);

  FeedTx u;
  if (!UringSender::built() || !u.open(Endpoint{"127.0.0.1", rx.port}, Transport::kUring, {}, {})) {
    GTEST_SKIP() << "io_uring unavailable";
  }
  EXPECT_TRUE(u.send(datagram(8, 50)));
  EXPECT_TRUE(u.drain());
  EXPECT_NE(u.report().find("1 completed"), std::string::npos) << u.report();
  const auto got = rx.drain();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], datagram(8, 50));
}

}  // namespace
