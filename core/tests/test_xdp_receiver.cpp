// AF_XDP feed reception (feed/xdp_receiver.hpp). The program and parser tests
// run everywhere: a small eBPF interpreter executes the exact instructions
// the kernel would load, with bounds checks on every packet read. The live
// tests need the veth pair of test_xdp.cpp (ADSB_XDP_TEST=TX_ADDR,RX_ADDR)
// and CAP_NET_ADMIN, CAP_BPF and CAP_NET_RAW; otherwise they are skipped.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <poll.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adsb/feed/net.hpp"
#include "adsb/feed/subscriber.hpp"
#include "adsb/feed/xdp_receiver.hpp"
#include "adsb/feed/xdp_sender.hpp"
#include "adsb/protocol/moldudp64.hpp"
#include "udp_receiver.hpp"

namespace {

using namespace adsb::feed;
using adsb::feed::testing::Receiver;
using adsb::protocol::PacketBuilder;
using namespace std::chrono_literals;

std::uint32_t ip_be(const char* dotted) {
  in_addr a{};
  EXPECT_EQ(::inet_pton(AF_INET, dotted, &a), 1);
  return a.s_addr;
}

// ------------------------------------------------------- eBPF interpreter

// Runs an XDP program on one frame. Only what the feed program may use is
// implemented; anything else (an unknown opcode, a read outside the packet
// or the context, a read of a register never written, running off the end)
// is a fault, as the verifier would reject it.
struct XdpRun {
  int action = -1;            // XDP_* the program returned
  std::uint32_t queue = 0;    // the key passed to bpf_redirect_map
  std::string fault;          // non-empty: the program misbehaved
};

XdpRun run_xdp(const std::vector<BpfInsn>& prog, std::span<const std::uint8_t> pkt, std::uint32_t rx_queue,
               const std::set<std::uint32_t>& sockets, int map_fd) {
  constexpr std::uint64_t kCtx = 0x10000, kPkt = 0x100000, kMap = 0xA000;
  std::uint64_t r[11] = {};
  unsigned init = 1u << 1 | 1u << 10;  // r1 = ctx, r10 = frame pointer
  r[1] = kCtx;
  XdpRun out;
  auto fault = [&](const std::string& why, std::size_t pc) {
    out.fault = why + " at " + std::to_string(pc);
    return out;
  };
  std::size_t pc = 0;
  for (int steps = 0; steps < 1000; ++steps) {
    if (pc >= prog.size()) return fault("ran off the end", pc);
    const BpfInsn& i = prog[pc];
    const unsigned d = i.regs & 0xF, s = i.regs >> 4;
    auto need = [&](unsigned reg) { return (init >> reg & 1) != 0; };
    auto set = [&](unsigned reg, std::uint64_t v) {
      r[reg] = v;
      init |= 1u << reg;
    };
    const std::uint64_t imm64 = static_cast<std::uint64_t>(static_cast<std::int64_t>(i.imm));
    switch (i.code) {
      case 0xbf:  // mov64 dst, src
        if (!need(s)) return fault("read of unset r" + std::to_string(s), pc);
        set(d, r[s]);
        break;
      case 0xb7: set(d, imm64); break;  // mov64 dst, imm
      case 0x07:                        // add64 dst, imm
        if (!need(d)) return fault("read of unset register", pc);
        r[d] += imm64;
        break;
      case 0x57:  // and64 dst, imm
        if (!need(d)) return fault("read of unset register", pc);
        r[d] &= imm64;
        break;
      case 0x61: case 0x69: case 0x71: {  // ldx w/h/b dst, [src + off]
        if (!need(s)) return fault("load through unset register", pc);
        const std::size_t size = i.code == 0x61 ? 4 : i.code == 0x69 ? 2 : 1;
        const std::uint64_t at = r[s] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i.off));
        if (at >= kPkt) {
          if (at + size > kPkt + pkt.size()) return fault("packet read out of bounds", pc);
          std::uint64_t v = 0;
          std::memcpy(&v, pkt.data() + (at - kPkt), size);  // host order, as in eBPF
          set(d, v);
        } else if (at >= kCtx && at < kCtx + 24) {
          if (size != 4) return fault("narrow context read", pc);
          const std::uint64_t field = at - kCtx;
          if (field == 0) set(d, kPkt);
          else if (field == 4) set(d, kPkt + pkt.size());
          else if (field == 16) set(d, rx_queue);
          else return fault("unexpected context field", pc);
        } else {
          return fault("read outside packet and context", pc);
        }
        break;
      }
      case 0x2d:  // jgt dst, src
        if (!need(d) || !need(s)) return fault("compare of unset register", pc);
        if (r[d] > r[s]) pc += static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i.off));
        break;
      case 0x55:  // jne dst, imm
        if (!need(d)) return fault("compare of unset register", pc);
        if (r[d] != imm64) pc += static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i.off));
        break;
      case 0x56:  // jne32 dst, imm
        if (!need(d)) return fault("compare of unset register", pc);
        if (static_cast<std::uint32_t>(r[d]) != static_cast<std::uint32_t>(i.imm))
          pc += static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i.off));
        break;
      case 0x18:  // ld_imm64 (two slots); only a pseudo map fd is expected
        if (pc + 1 >= prog.size() || prog[pc + 1].code != 0) return fault("bad ld_imm64", pc);
        if (s != kBpfPseudoMapFd || i.imm != map_fd) return fault("ld_imm64 is not our map", pc);
        set(d, kMap);
        ++pc;
        break;
      case 0x85: {  // call bpf_redirect_map(map, key, flags)
        if (i.imm != kBpfFuncRedirectMap) return fault("unexpected helper", pc);
        if (!need(1) || !need(2) || !need(3) || r[1] != kMap) return fault("bad helper arguments", pc);
        out.queue = static_cast<std::uint32_t>(r[2]);
        // No socket at that key: the low bits of flags are the action.
        set(0, sockets.count(out.queue) != 0 ? static_cast<std::uint64_t>(kXdpRedirect) : (r[3] & 0x3));
        init &= ~(0x3Eu);  // r1-r5 are clobbered by a call
        break;
      }
      case 0x95:  // exit
        if (!need(0)) return fault("exit without r0", pc);
        out.action = static_cast<int>(r[0]);
        return out;
      default:
        return fault("unknown opcode " + std::to_string(i.code), pc);
    }
    ++pc;
  }
  return fault("too many steps", pc);
}

// A feed-like frame from the publisher's frame builder.
std::vector<std::uint8_t> frame(const char* dst_ip, std::uint16_t dst_port, std::size_t payload = 40) {
  UdpFrameBuilder b;
  b.init(MacAddress{2, 0, 0, 0, 0, 1}, MacAddress{2, 0, 0, 0, 0, 2}, ip_be("10.11.0.1"), ip_be(dst_ip), 5000,
         dst_port, 8);
  std::vector<std::uint8_t> data(payload);
  for (std::size_t i = 0; i < payload; ++i) data[i] = static_cast<std::uint8_t>(i * 7 + 1);
  std::vector<std::uint8_t> f(UdpFrameBuilder::kHeaderBytes + payload);
  f.resize(b.build(data, f.data()));
  return f;
}

// Recomputes the IPv4 header checksum after a test edited the header.
void fix_ip_checksum(std::vector<std::uint8_t>& f) {
  f[24] = f[25] = 0;
  const std::uint16_t c = inet_checksum(std::span<const std::uint8_t>(f).subspan(14, 20));
  f[24] = static_cast<std::uint8_t>(c >> 8);
  f[25] = static_cast<std::uint8_t>(c);
}

constexpr int kMapFd = 7;

XdpRun run_on(const std::vector<std::uint8_t>& f, const char* match_ip = "10.11.0.200",
              std::uint16_t port = 41900, std::uint32_t queue = 0, std::set<std::uint32_t> sockets = {0}) {
  const auto prog = xdp_feed_program(kMapFd, match_ip ? ip_be(match_ip) : 0, port);
  return run_xdp(prog, f, queue, sockets, kMapFd);
}

// ------------------------------------------------------------ portable

TEST(XdpProgram, RedirectsTheFeedToTheSocketOnItsQueue) {
  // .200: the address's last byte has the high bit set, so a sign-extended
  // 64-bit compare would never match (the program uses a 32-bit compare).
  const auto f = frame("10.11.0.200", 41900);
  XdpRun r = run_on(f);
  ASSERT_TRUE(r.fault.empty()) << r.fault;
  EXPECT_EQ(r.action, kXdpRedirect);
  EXPECT_EQ(r.queue, 0u);
  r = run_on(f, "10.11.0.200", 41900, 3, {3});
  ASSERT_TRUE(r.fault.empty()) << r.fault;
  EXPECT_EQ(r.action, kXdpRedirect);
  EXPECT_EQ(r.queue, 3u) << "the key is the frame's RX queue";
  // Multicast groups and ports with either byte above 0x7F.
  for (const char* g : {"239.19.0.1", "239.255.128.255", "224.0.0.251"}) {
    for (const std::uint16_t port : std::vector<std::uint16_t>{1, 255, 256, 41900, 65535}) {
      r = run_on(frame(g, port), g, port);
      ASSERT_TRUE(r.fault.empty()) << r.fault;
      EXPECT_EQ(r.action, kXdpRedirect) << g << ":" << port;
    }
  }
}

TEST(XdpProgram, PassesWhenNoSocketHoldsTheQueue) {
  const XdpRun r = run_on(frame("10.11.0.200", 41900), "10.11.0.200", 41900, 1, {0});
  ASSERT_TRUE(r.fault.empty()) << r.fault;
  EXPECT_EQ(r.action, kXdpPass) << "bpf_redirect_map's fallback";
}

TEST(XdpProgram, PassesEverythingElseToTheKernel) {
  auto expect_pass = [](std::vector<std::uint8_t> f, const char* what, const char* ip = "10.11.0.200") {
    const XdpRun r = run_on(f, ip);
    ASSERT_TRUE(r.fault.empty()) << what << ": " << r.fault;
    EXPECT_EQ(r.action, kXdpPass) << what;
  };
  expect_pass(frame("10.11.0.200", 41901), "another port");
  expect_pass(frame("10.11.0.201", 41900), "another address");
  expect_pass(frame("10.11.0.72", 41900), "an address differing only in the high byte of the load");
  auto f = frame("10.11.0.200", 41900);
  f[12] = 0x08, f[13] = 0x06;
  expect_pass(f, "ARP");
  f = frame("10.11.0.200", 41900);
  f[12] = 0x86, f[13] = 0xDD;
  expect_pass(f, "IPv6");
  f = frame("10.11.0.200", 41900);
  f[14] = 0x46;
  expect_pass(f, "IP options");
  f = frame("10.11.0.200", 41900);
  f[14] = 0x65;
  expect_pass(f, "IP version 6 in an IPv4 frame");
  f = frame("10.11.0.200", 41900);
  f[23] = 6;
  expect_pass(f, "TCP");
  f = frame("10.11.0.200", 41900);
  f[20] |= 0x20;
  expect_pass(f, "more fragments");
  f = frame("10.11.0.200", 41900);
  f[21] = 0x01;
  expect_pass(f, "a fragment offset");
  f = frame("10.11.0.200", 41900);
  f[20] = 0x40, f[21] = 0;  // DF alone is not a fragment
  const XdpRun r = run_on(f);
  ASSERT_TRUE(r.fault.empty()) << r.fault;
  EXPECT_EQ(r.action, kXdpRedirect) << "DF set";
}

TEST(XdpProgram, NeverReadsPastAShortFrame) {
  const auto full = frame("10.11.0.200", 41900, 0);
  ASSERT_EQ(full.size(), kUdpFrameHeaderBytes);
  for (std::size_t n = 0; n < full.size(); ++n) {
    const std::vector<std::uint8_t> f(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
    const XdpRun r = run_on(f);
    ASSERT_TRUE(r.fault.empty()) << n << " bytes: " << r.fault;
    EXPECT_EQ(r.action, kXdpPass) << n << " bytes";
  }
  const XdpRun r = run_on(full);
  ASSERT_TRUE(r.fault.empty()) << r.fault;
  EXPECT_EQ(r.action, kXdpRedirect) << "headers and an empty payload";
}

TEST(XdpProgram, AnyAddressMatchesOnThePortAlone) {
  for (const char* ip : {"10.11.0.200", "239.19.0.1", "192.0.2.9"}) {
    const XdpRun r = run_on(frame(ip, 41900), nullptr);
    ASSERT_TRUE(r.fault.empty()) << r.fault;
    EXPECT_EQ(r.action, kXdpRedirect) << ip;
  }
  EXPECT_LT(xdp_feed_program(kMapFd, 0, 1).size(), xdp_feed_program(kMapFd, ip_be("10.0.0.1"), 1).size());
}

TEST(XdpProgram, AgreesWithTheParserOnEveryHeaderByte) {
  // Flip each header byte through a few values: the program redirects
  // exactly the frames whose headers the parser accepts (it does not check
  // the IP and UDP lengths, which only the parser does: the kernel delivers
  // what the device received).
  const auto base = frame("10.11.0.200", 41900, 24);
  const std::uint32_t ip = ip_be("10.11.0.200");
  for (std::size_t at = 0; at < kUdpFrameHeaderBytes; ++at) {
    for (std::uint8_t v : {std::uint8_t{0x00}, std::uint8_t{0x45}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      auto f = base;
      f[at] = v;
      const XdpRun r = run_on(f);
      ASSERT_TRUE(r.fault.empty()) << r.fault;
      std::span<const std::uint8_t> payload;
      const FrameCheck c = udp_payload(f, ip, 41900, payload);
      // The program does not check lengths or the checksum: receive() does.
      const bool header_ok = c == FrameCheck::kOk || c == FrameCheck::kBadLength || c == FrameCheck::kBadChecksum;
      EXPECT_EQ(r.action == kXdpRedirect, header_ok) << "byte " << at << " = " << int{v} << ": " << to_string(c);
    }
  }
}

TEST(XdpFrameParser, ReturnsThePayloadAndExplainsRejections) {
  const std::uint32_t ip = ip_be("239.19.0.1");
  auto f = frame("239.19.0.1", 41900, 100);
  std::span<const std::uint8_t> p;
  ASSERT_EQ(udp_payload(f, ip, 41900, p), FrameCheck::kOk);
  ASSERT_EQ(p.size(), 100u);
  EXPECT_EQ(p.data(), f.data() + kUdpFrameHeaderBytes);
  EXPECT_EQ(p[1], 8);
  EXPECT_EQ(udp_payload(f, 0, 41900, p), FrameCheck::kOk) << "any address";
  // Ethernet pads short frames to 60 bytes: the UDP length decides.
  auto small = frame("239.19.0.1", 41900, 2);
  small.resize(60, 0);
  ASSERT_EQ(udp_payload(small, ip, 41900, p), FrameCheck::kOk);
  EXPECT_EQ(p.size(), 2u);

  EXPECT_EQ(udp_payload(std::span(f).first(41), ip, 41900, p), FrameCheck::kShort);
  EXPECT_TRUE(p.empty());
  auto g = f;
  g[13] = 0x06;
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kNotIpv4);
  g = f;
  g[14] = 0x47;
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kIpOptions);
  g = f;
  g[20] = 0x20;
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kFragment);
  g = f;
  g[23] = 1;
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kNotUdp);
  EXPECT_EQ(udp_payload(f, ip, 41901, p), FrameCheck::kWrongDest);
  EXPECT_EQ(udp_payload(f, ip_be("239.19.0.2"), 41900, p), FrameCheck::kWrongDest);
  g = f;
  g[24] ^= 0x01;  // the IPv4 header checksum
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kBadChecksum);
  g = f;
  g[22] = 1;  // TTL changed without fixing the checksum
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kBadChecksum);
  g = f;
  g.pop_back();  // IP total length now exceeds the frame
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kBadLength);
  g = f;
  g[39] = static_cast<std::uint8_t>(g[39] + 1);  // UDP length longer than the IP length allows
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kBadLength);
  g = f;
  g[39] = static_cast<std::uint8_t>(g[39] - 2);  // and shorter
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kBadLength);
  g = f;
  g[16] = 0, g[17] = 27;  // IP length below the headers
  fix_ip_checksum(g);
  EXPECT_EQ(udp_payload(g, ip, 41900, p), FrameCheck::kBadLength);
}

TEST(XdpReceiver, ParsesAttachModes) {
  XdpReceiver::Attach a{};
  ASSERT_TRUE(XdpReceiver::parse_attach("driver", a));
  EXPECT_EQ(a, XdpReceiver::Attach::kDriver);
  ASSERT_TRUE(XdpReceiver::parse_attach("skb", a));
  EXPECT_EQ(a, XdpReceiver::Attach::kGeneric);
  ASSERT_TRUE(XdpReceiver::parse_attach("auto", a));
  EXPECT_EQ(a, XdpReceiver::Attach::kAuto);
  EXPECT_FALSE(XdpReceiver::parse_attach("fast", a));
  EXPECT_STREQ(XdpReceiver::attach_name(XdpReceiver::Attach::kGeneric), "generic");
}

TEST(XdpReceiver, FailsCleanlyWhenItCannotOpen) {
  XdpReceiver x;
  std::string err;
  XdpReceiver::Options o;
  o.interface_host = "127.0.0.1";
  o.frames = 100;
  EXPECT_FALSE(x.open(Endpoint{"239.19.0.1", 41900}, o, &err));
  EXPECT_FALSE(err.empty());
  EXPECT_FALSE(x.is_open());
  EXPECT_EQ(x.fd(), -1);
  int calls = 0;
  auto fn = [&](std::span<const std::uint8_t>) { ++calls; };
  EXPECT_EQ(x.receive(fn, 10), 0u);
  EXPECT_EQ(calls, 0);
}

// The kernel path's counter (the AF_XDP checks only ever see it at 0): over
// loopback, every feed datagram is counted as received by the kernel socket.
TEST(XdpReceiver, SubscriberClientCountsKernelDatagramsWithoutAfXdp) {
  SubscriberClientConfig cfg;
  cfg.feed = Endpoint{"127.0.0.1", 0};
  SubscriberClient sub(cfg);
  std::string err;
  ASSERT_TRUE(sub.open(&err)) << err;
  EXPECT_FALSE(sub.xdp().is_open());
  UdpSender tx;
  ASSERT_TRUE(tx.open(Endpoint{"127.0.0.1", sub.feed_port()}, UdpSender::Options{}, &err)) << err;
  std::atomic<bool> stop{false}, done{false};
  std::thread run([&] {
    sub.run(stop, /*stop_at_end=*/true);
    done = true;
  });
  const auto session = adsb::protocol::make_session("KERNRX0001");
  constexpr std::uint64_t kPackets = 50;
  for (std::uint64_t q = 1; q <= kPackets; ++q) {
    PacketBuilder b(session, q);
    adsb::protocol::TrackUpdate u;
    u.icao = static_cast<std::uint32_t>(q);
    std::uint8_t m[adsb::protocol::TrackUpdate::kWireSize];
    adsb::protocol::write(u, m);
    ASSERT_TRUE(b.add(m, sizeof m));
    ASSERT_TRUE(tx.send(b.finish()));
  }
  PacketBuilder end(session, kPackets + 1);
  ASSERT_TRUE(tx.send(end.heartbeat(true)));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  stop = true;
  run.join();
  EXPECT_TRUE(sub.core().ended());
  EXPECT_EQ(sub.feed_via_kernel(), kPackets + 1);
  EXPECT_EQ(sub.feed_via_xdp(), 0u);
}

// ---------------------------------------------------------------- live

struct Pair {
  std::string tx, rx;
};

#define PAIR_OR_SKIP(pair)                                                                 \
  Pair pair;                                                                               \
  do {                                                                                     \
    if (!XdpReceiver::built()) GTEST_SKIP() << "built without AF_XDP";                    \
    const char* env_ = std::getenv("ADSB_XDP_TEST");                                       \
    if (!env_ || !std::strchr(env_, ',')) GTEST_SKIP() << "set ADSB_XDP_TEST=TX_ADDR,RX_ADDR"; \
    const std::string s_ = env_;                                                           \
    pair.tx = s_.substr(0, s_.find(','));                                                  \
    pair.rx = s_.substr(s_.find(',') + 1);                                                 \
  } while (0)

std::vector<std::uint8_t> datagram(std::uint64_t seq, std::size_t len) {
  std::vector<std::uint8_t> d(len);
  for (std::size_t i = 0; i < len; ++i) d[i] = static_cast<std::uint8_t>(seq * 31 + i);
  std::memcpy(d.data(), &seq, 8);
  return d;
}

// Receives on its own thread (the XSK's single user), like udp_receiver.hpp.
class XskCollector {
 public:
  explicit XskCollector(XdpReceiver& x) : x_(x) {
    thread_ = std::thread([this] {
      auto keep = [this](std::span<const std::uint8_t> p) {
        got_.emplace_back(p.begin(), p.end());
        count_.fetch_add(1);
      };
      pollfd pfd{x_.fd(), POLLIN, 0};
      while (!stop_.load()) {
        (void)::poll(&pfd, 1, 1);
        (void)x_.receive(keep, 256);
      }
    });
  }
  ~XskCollector() { join(); }
  bool wait_count(std::uint64_t n, std::chrono::milliseconds timeout = 3s) const {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (count_.load() < n) {
      if (std::chrono::steady_clock::now() > end) return false;
      std::this_thread::yield();
    }
    return true;
  }
  // Stops the thread; then got() is safe to read.
  void join() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  const std::vector<std::vector<std::uint8_t>>& got() const { return got_; }

 private:
  XdpReceiver& x_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> count_{0};
  std::vector<std::vector<std::uint8_t>> got_;
  std::thread thread_;
};

XdpSender::Options tx_options(const Pair& p) {
  XdpSender::Options o;
  o.udp.interface_host = p.tx;
  o.mode = XdpSender::Mode::kCopy;
  return o;
}

XdpReceiver::Options rx_options(const Pair& p) {
  XdpReceiver::Options o;
  o.interface_host = p.rx;
  return o;
}

// Sends n datagrams in windows; every one must come out of the XSK intact,
// once, in order, and none through the kernel socket on the same port.
void send_and_check(XdpSender& tx, XskCollector& xsk, Receiver& kernel, std::uint64_t n) {
  for (std::uint64_t s = 1; s <= n; ++s) {
    ASSERT_TRUE(tx.send(datagram(s, 16 + s % 1400)));
    if (s % 64 == 0) {
      ASSERT_TRUE(xsk.wait_count(s)) << "lost before " << s;
    }
  }
  ASSERT_TRUE(tx.drain());
  ASSERT_TRUE(xsk.wait_count(n));
  xsk.join();
  ASSERT_EQ(xsk.got().size(), n);
  for (std::uint64_t s = 1; s <= n; ++s) ASSERT_EQ(xsk.got()[s - 1], datagram(s, 16 + s % 1400)) << s;
  EXPECT_TRUE(kernel.drain(100ms).empty()) << "redirected frames must not also reach the kernel";
}

TEST(XdpReceiver, UnicastFeedArrivesThroughTheSocket) {
  PAIR_OR_SKIP(p);
  Receiver kernel(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  ASSERT_TRUE(kernel.ok());
  XdpReceiver x;
  std::string err;
  ASSERT_TRUE(x.open(Endpoint{p.rx, kernel.port}, rx_options(p), &err)) << err;
  EXPECT_EQ(x.bound_mode(), XdpReceiver::Mode::kCopy) << "veth has no zero-copy";
  EXPECT_NE(x.attach_mode(), XdpReceiver::Attach::kAuto);
  XskCollector xsk(x);
  XdpSender tx;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, kernel.port}, tx_options(p), &err)) << err;
  send_and_check(tx, xsk, kernel, 3000);
  x.refresh_kernel_stats();
  EXPECT_EQ(x.stats().received, 3000u);
  EXPECT_EQ(x.stats().rejected, 0u);
  EXPECT_EQ(x.stats().rx_dropped, 0u);
  EXPECT_EQ(x.stats().rx_invalid, 0u);
}

TEST(XdpReceiver, MulticastFeedArrivesThroughTheSocket) {
  PAIR_OR_SKIP(p);
  Receiver kernel(Receiver::Adopt{}, open_feed_receiver(Endpoint{"239.19.0.78", 0}, p.rx, 8 << 20));
  ASSERT_TRUE(kernel.ok());
  XdpReceiver x;
  std::string err;
  ASSERT_TRUE(x.open(Endpoint{"239.19.0.78", kernel.port}, rx_options(p), &err)) << err;
  XskCollector xsk(x);
  XdpSender tx;
  ASSERT_TRUE(tx.open(Endpoint{"239.19.0.78", kernel.port}, tx_options(p), &err)) << err;
  send_and_check(tx, xsk, kernel, 3000);
}

TEST(XdpReceiver, OtherTrafficStillReachesTheKernel) {
  PAIR_OR_SKIP(p);
  Receiver feed_sock(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  Receiver other(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  ASSERT_TRUE(feed_sock.ok() && other.ok());
  XdpReceiver x;
  std::string err;
  ASSERT_TRUE(x.open(Endpoint{p.rx, feed_sock.port}, rx_options(p), &err)) << err;
  XskCollector xsk(x);
  XdpSender tx;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, other.port}, tx_options(p), &err)) << err;
  for (std::uint64_t s = 1; s <= 200; ++s) ASSERT_TRUE(tx.send(datagram(s, 100)));
  ASSERT_TRUE(tx.drain());
  ASSERT_TRUE(other.wait_count(200));
  xsk.join();
  EXPECT_TRUE(xsk.got().empty()) << "another port is not the feed";
}

TEST(XdpReceiver, ClosingDetachesTheProgram) {
  PAIR_OR_SKIP(p);
  Receiver kernel(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  ASSERT_TRUE(kernel.ok());
  std::string err;
  XdpSender tx;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, kernel.port}, tx_options(p), &err)) << err;
  {
    XdpReceiver x;
    ASSERT_TRUE(x.open(Endpoint{p.rx, kernel.port}, rx_options(p), &err)) << err;
    XskCollector xsk(x);
    ASSERT_TRUE(tx.send(datagram(1, 64)));
    ASSERT_TRUE(tx.drain());
    ASSERT_TRUE(xsk.wait_count(1));
  }
  // The link is gone with the receiver: the kernel socket gets the feed again.
  ASSERT_TRUE(tx.send(datagram(2, 64)));
  ASSERT_TRUE(tx.drain());
  ASSERT_TRUE(kernel.wait_count(1)) << "the program stayed attached after close";
  // And a new receiver can attach at once (the queue's release is deferred).
  XdpReceiver again;
  ASSERT_TRUE(again.open(Endpoint{p.rx, kernel.port}, rx_options(p), &err)) << err;
}

TEST(XdpReceiver, ASecondReceiverOnTheDeviceIsRefusedClearly) {
  PAIR_OR_SKIP(p);
  XdpReceiver a, b;
  std::string err;
  ASSERT_TRUE(a.open(Endpoint{p.rx, 41977}, rx_options(p), &err)) << err;
  EXPECT_FALSE(b.open(Endpoint{p.rx, 41978}, rx_options(p), &err));
  EXPECT_NE(err.find("another"), std::string::npos) << err;
  EXPECT_FALSE(b.is_open());
}

TEST(XdpReceiver, SubscriberClientRecoversAFeedReceivedOverAfXdp) {
  PAIR_OR_SKIP(p);
  // The publisher side: feed packets for messages 1..N, then end of session,
  // sent over AF_XDP from the far end of the pair (no loss, so neither the
  // retransmit nor the snapshot server is asked).
  SubscriberClientConfig cfg;
  cfg.feed = Endpoint{"239.19.0.79", 0};
  cfg.interface_host = p.rx;
  cfg.rx = SubscriberClientConfig::Rx::kXdp;
  SubscriberClient sub(cfg);
  std::string err;
  ASSERT_TRUE(sub.open(&err)) << err;
  ASSERT_TRUE(sub.xdp().is_open());
  XdpSender tx;
  ASSERT_TRUE(tx.open(Endpoint{"239.19.0.79", sub.feed_port()}, tx_options(p), &err)) << err;
  std::atomic<bool> stop{false}, done{false};
  std::thread run([&] {
    sub.run(stop, /*stop_at_end=*/true);
    done = true;
  });
  const auto session = adsb::protocol::make_session("XDPRX00001");
  constexpr std::uint64_t kN = 5000;
  FeedBook expect;
  std::uint64_t seq = 1, packets = 0;
  while (seq <= kN) {
    PacketBuilder b(session, seq);
    for (int k = 0; k < 20 && seq <= kN; ++k, ++seq) {
      adsb::protocol::TrackUpdate u;
      u.icao = static_cast<std::uint32_t>(seq % 97 + 1);
      u.alt_ft = static_cast<std::int32_t>(seq);
      u.event_ts_ns = seq;
      std::uint8_t m[adsb::protocol::TrackUpdate::kWireSize];
      adsb::protocol::write(u, m);
      ASSERT_TRUE(b.add(m, sizeof m));
      expect.apply(std::span<const std::uint8_t>(m, sizeof m));
    }
    ASSERT_TRUE(tx.send(b.finish()));
    if (++packets % 32 == 0) std::this_thread::sleep_for(1ms);  // within the receive rings
  }
  PacketBuilder end(session, kN + 1);
  ASSERT_TRUE(tx.send(end.heartbeat(true)));
  ASSERT_TRUE(tx.drain());
  // run() returns once the session has ended; give up after the deadline.
  // The core is read only after join (it belongs to the run thread).
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!done.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
  stop = true;
  run.join();
  EXPECT_TRUE(sub.core().ended());
  EXPECT_EQ(sub.core().expected(), kN + 1);
  EXPECT_TRUE(sub.core().book() == expect);
  EXPECT_EQ(sub.feed_via_xdp(), packets + 1);
  EXPECT_EQ(sub.feed_via_kernel(), 0u);
  EXPECT_EQ(sub.core().stats().gaps, 0u);
}

}  // namespace
