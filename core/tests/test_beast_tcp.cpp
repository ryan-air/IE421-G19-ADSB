// Live Beast TCP ingest, against a real TCP server running inside the test.

#include "adsb/ingest/beast_tcp.hpp"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "adsb/common/rng.hpp"

namespace {

using namespace adsb;
using namespace adsb::ingest;
using namespace std::chrono_literals;

// A listening socket on 127.0.0.1 with an OS-assigned port.
class Listener {
 public:
  Listener() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    EXPECT_EQ(::listen(fd_, 4), 0);
    socklen_t len = sizeof a;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
  }
  ~Listener() { ::close(fd_); }
  std::uint16_t port() const { return port_; }
  int accept_one() { return ::accept(fd_, nullptr, nullptr); }

 private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
};

// Writes `bytes` to a connected socket in random chunk sizes, with tiny pauses
// so frames are split across recv() calls on the reader side.
void send_chunked(int fd, const std::vector<std::uint8_t>& bytes, std::uint64_t seed) {
  Rng rng(seed);
  std::size_t off = 0;
  while (off < bytes.size()) {
    const std::size_t n = std::min<std::size_t>(1 + rng.below(700), bytes.size() - off);
    ASSERT_EQ(::send(fd, bytes.data() + off, n, 0), static_cast<ssize_t>(n));
    off += n;
    if (rng.below(8) == 0) std::this_thread::sleep_for(200us);
  }
}

struct Stream {
  std::vector<BeastFrame> frames;
  std::vector<std::uint8_t> bytes;
  std::vector<std::size_t> frame_end;  // byte offset just after each frame
};

Stream make_stream(int n, std::uint64_t seed) {
  Rng rng(seed);
  Stream s;
  for (int i = 0; i < n; ++i) {
    BeastFrame f;
    f.type = rng.below(2) ? BeastType::kModeSLong : BeastType::kModeSShort;
    f.len = f.type == BeastType::kModeSLong ? 14 : 7;
    f.timestamp = (rng.below(4) == 0) ? 0x1A1A1A1A1A1AULL : (rng.next() & 0xFFFFFFFFFFFFULL);
    f.signal = static_cast<std::uint8_t>(rng.next());
    for (int k = 0; k < f.len; ++k) {
      f.payload[k] = rng.below(5) == 0 ? 0x1A : static_cast<std::uint8_t>(rng.next());
    }
    std::uint8_t buf[kBeastMaxEncodedLen];
    const std::size_t len = beast_encode(f, buf);
    s.bytes.insert(s.bytes.end(), buf, buf + len);
    s.frame_end.push_back(s.bytes.size());
    s.frames.push_back(f);
  }
  return s;
}

bool same(const record::Record& r, const BeastFrame& f) {
  return r.type == static_cast<std::uint8_t>(f.type) && r.len == f.len && r.signal == f.signal &&
         r.radio_ts == f.timestamp && std::memcmp(r.payload, f.payload, f.len) == 0;
}

// Runs a source on its own thread until `want` frames arrived or `timeout`.
struct Runner {
  BeastTcpSource source;
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::vector<record::Record> got;
  std::thread th;

  explicit Runner(BeastTcpConfig cfg) : source(std::move(cfg)) {
    th = std::thread([this] {
      source.run(stop, [this](const record::Record& r) {
        std::lock_guard<std::mutex> l(mu);
        got.push_back(r);
      });
    });
  }
  bool wait_for(std::size_t want, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      {
        std::lock_guard<std::mutex> l(mu);
        if (got.size() >= want) return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }
  void finish() {
    stop = true;
    if (th.joinable()) th.join();
  }
  ~Runner() { finish(); }
};

BeastTcpConfig config_for(std::uint16_t port, bool busy) {
  BeastTcpConfig c;
  c.host = "127.0.0.1";
  c.port = port;
  c.line = 3;
  c.busy_poll = busy;
  c.reconnect_initial_ms = 20;
  c.reconnect_max_ms = 100;
  return c;
}

void receives_everything(bool busy_poll) {
  Listener l;
  const Stream s = make_stream(3000, 11);
  Runner run(config_for(l.port(), busy_poll));
  const int conn = l.accept_one();
  ASSERT_GE(conn, 0);
  send_chunked(conn, s.bytes, 5);

  ASSERT_TRUE(run.wait_for(s.frames.size(), 10s));
  run.finish();
  ::close(conn);

  ASSERT_EQ(run.got.size(), s.frames.size());
  std::uint64_t prev_rx = 0;
  for (std::size_t i = 0; i < s.frames.size(); ++i) {
    ASSERT_TRUE(same(run.got[i], s.frames[i])) << "frame " << i;
    EXPECT_EQ(run.got[i].line, 3);
    ASSERT_GE(run.got[i].rx_ns, prev_rx) << "receive time must not go backwards";
    prev_rx = run.got[i].rx_ns;
  }
  const auto st = run.source.stats();
  EXPECT_EQ(st.connects, 1u);
  EXPECT_EQ(st.frames, s.frames.size());
  EXPECT_EQ(st.bytes, s.bytes.size());
  EXPECT_EQ(st.parser.truncated, 0u);
}

TEST(BeastTcp, ReceivesEveryFrameAcrossArbitraryChunking) { receives_everything(false); }
TEST(BeastTcp, BusyPollModeReceivesEveryFrame) { receives_everything(true); }

TEST(BeastTcp, ReconnectsAndDropsTheFrameCutByTheDisconnect) {
  Listener l;
  const Stream s = make_stream(400, 21);
  // First connection: send frames 0..199 plus half of frame 200, then drop.
  const std::size_t cut = s.frame_end[199] + (s.frame_end[200] - s.frame_end[199]) / 2;
  Runner run(config_for(l.port(), false));
  int c1 = l.accept_one();
  ASSERT_GE(c1, 0);
  send_chunked(c1, std::vector<std::uint8_t>(s.bytes.begin(), s.bytes.begin() + static_cast<std::ptrdiff_t>(cut)), 1);
  ASSERT_TRUE(run.wait_for(200, 5s));
  ::close(c1);

  // Second connection: resume at frame 201 (frame 200 is lost, as it would be).
  int c2 = l.accept_one();
  ASSERT_GE(c2, 0);
  send_chunked(c2, std::vector<std::uint8_t>(s.bytes.begin() + static_cast<std::ptrdiff_t>(s.frame_end[200]), s.bytes.end()), 2);
  ASSERT_TRUE(run.wait_for(399, 5s));
  run.finish();
  ::close(c2);

  ASSERT_EQ(run.got.size(), 399u) << "exactly the cut frame is missing";
  for (std::size_t i = 0; i < 200; ++i) ASSERT_TRUE(same(run.got[i], s.frames[i])) << i;
  for (std::size_t i = 200; i < 399; ++i) ASSERT_TRUE(same(run.got[i], s.frames[i + 1])) << i;
  const auto st = run.source.stats();
  EXPECT_EQ(st.connects, 2u);
  EXPECT_EQ(st.disconnects, 1u);
}

TEST(BeastTcp, BacksOffWhenNothingIsListeningAndStopsPromptly) {
  std::uint16_t port;
  {
    Listener l;  // grab a free port, then close it so connects are refused
    port = l.port();
  }
  Runner run(config_for(port, false));
  std::this_thread::sleep_for(300ms);
  const auto t0 = std::chrono::steady_clock::now();
  run.finish();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms) << "stop is honoured during backoff";
  const auto st = run.source.stats();
  EXPECT_EQ(st.connects, 0u);
  EXPECT_GE(st.connect_failures, 2u) << "it keeps retrying";
  EXPECT_LE(st.connect_failures, 20u) << "with exponential backoff, not a tight loop";
}

TEST(BeastTcp, StopsPromptlyWhileConnectedAndIdle) {
  for (const bool busy : {false, true}) {
    Listener l;
    Runner run(config_for(l.port(), busy));
    const int conn = l.accept_one();
    ASSERT_GE(conn, 0);
    std::this_thread::sleep_for(150ms);
    const auto t0 = std::chrono::steady_clock::now();
    run.finish();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 400ms) << "busy=" << busy;
    ::close(conn);
  }
}

TEST(BeastTcp, ReportsResolutionFailure) {
  BeastTcpConfig c = config_for(30005, false);
  c.host = "no-such-host.invalid";
  Runner run(c);
  std::this_thread::sleep_for(200ms);
  run.finish();
  EXPECT_GE(run.source.stats().connect_failures, 1u);
}

}  // namespace
