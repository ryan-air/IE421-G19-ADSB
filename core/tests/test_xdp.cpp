// AF_XDP feed transport (feed/xdp_sender.hpp). The frame builder tests run
// everywhere. The live tests need a device pair on Linux and CAP_NET_RAW:
// set ADSB_XDP_TEST=TX_ADDR,RX_ADDR, two addresses on the two ends of a veth
// pair, with an ARP entry for RX_ADDR on the TX device (see the CI job
// `xdp`); otherwise they are skipped.

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "adsb/feed/net.hpp"
#include "adsb/feed/tx.hpp"
#include "adsb/feed/xdp_sender.hpp"
#include "udp_receiver.hpp"

namespace {

using namespace adsb::feed;
using adsb::feed::testing::Receiver;
using namespace std::chrono_literals;

std::uint32_t ip_be(const char* dotted) {
  in_addr a{};
  EXPECT_EQ(::inet_pton(AF_INET, dotted, &a), 1);
  return a.s_addr;
}

std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

// An independent, deliberately plain UDP checksum check: the one's
// complement sum over pseudo-header + UDP header + data, checksum included,
// must be 0xFFFF.
bool udp_checksum_ok(const std::uint8_t* frame, std::size_t len) {
  const std::uint8_t* ip = frame + 14;
  const std::uint8_t* udp = ip + 20;
  const std::size_t udp_len = len - 34;
  std::uint32_t sum = 0;
  for (int i = 12; i < 20; i += 2) sum += get16(ip + i);
  sum += 17;
  sum += static_cast<std::uint32_t>(udp_len);
  for (std::size_t i = 0; i < udp_len; i += 2) {
    sum += static_cast<std::uint32_t>(udp[i] << 8) + (i + 1 < udp_len ? udp[i + 1] : 0u);
  }
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return sum == 0xFFFF;
}

TEST(XdpFrame, ParsesAndPrintsMacAddresses) {
  MacAddress m{};
  ASSERT_TRUE(parse_mac("02:aB:cd:EF:00:9f", m));
  EXPECT_EQ(to_string(m), "02:ab:cd:ef:00:9f");
  EXPECT_FALSE(parse_mac("02:ab:cd:ef:00", m));
  EXPECT_FALSE(parse_mac("02-ab-cd-ef-00-9f", m));
  EXPECT_FALSE(parse_mac("02:ab:cd:ef:00:9g", m));
}

TEST(XdpFrame, MulticastMacKeepsTheLow23BitsOfTheGroup) {
  EXPECT_EQ(to_string(multicast_mac(ip_be("239.19.0.7"))), "01:00:5e:13:00:07");
  // The top bit of the second octet is dropped (RFC 1112: 32 groups per MAC).
  EXPECT_EQ(to_string(multicast_mac(ip_be("239.147.0.7"))), "01:00:5e:13:00:07");
  EXPECT_EQ(to_string(multicast_mac(ip_be("224.0.0.251"))), "01:00:5e:00:00:fb");
}

TEST(XdpFrame, InternetChecksumMatchesRfc1071) {
  // RFC 1071 §3: these bytes sum to 0xddf2 (folded); the checksum is ~0xddf2.
  const std::uint8_t b[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
  EXPECT_EQ(inet_checksum(b), 0x220d);
  // Odd length: the last byte is padded with zero.
  const std::uint8_t c[] = {0x01};
  EXPECT_EQ(inet_checksum(c), static_cast<std::uint16_t>(~0x0100));
}

TEST(XdpFrame, BuildsTheIpv4HeaderOfAKnownPacket) {
  // A widely used example header (192.168.0.1 -> 192.168.0.199, UDP, TTL 64,
  // DF, id 0, total length 115): checksum 0xb861.
  const std::uint8_t want[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                                 0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
  UdpFrameBuilder b;
  const MacAddress src{2, 0, 0, 0, 0, 1}, dst{2, 0, 0, 0, 0, 2};
  b.init(src, dst, ip_be("192.168.0.1"), ip_be("192.168.0.199"), 5000, 6000, 64);
  std::vector<std::uint8_t> payload(115 - 28, 0x5a), frame(2048);
  const std::size_t len = b.build(payload, frame.data());
  ASSERT_EQ(len, 14 + 115u);
  EXPECT_EQ(std::memcmp(frame.data() + 14, want, 20), 0);
  EXPECT_EQ(std::memcmp(frame.data(), dst.data(), 6), 0);
  EXPECT_EQ(std::memcmp(frame.data() + 6, src.data(), 6), 0);
  EXPECT_EQ(get16(frame.data() + 12), 0x0800);
  EXPECT_EQ(get16(frame.data() + 34), 5000);
  EXPECT_EQ(get16(frame.data() + 36), 6000);
  EXPECT_EQ(get16(frame.data() + 38), 8 + payload.size());
  EXPECT_EQ(std::memcmp(frame.data() + 42, payload.data(), payload.size()), 0);
  EXPECT_TRUE(udp_checksum_ok(frame.data(), len));
}

TEST(XdpFrame, UdpChecksumIsRightForEveryLengthAndNeverZero) {
  UdpFrameBuilder b;
  b.init({2, 0, 0, 0, 0, 1}, multicast_mac(ip_be("239.19.0.7")), ip_be("10.11.0.1"), ip_be("239.19.0.7"),
         42900, 42900, 1);
  std::vector<std::uint8_t> frame(2048);
  for (std::size_t n = 1; n <= 1472; n += (n < 64 ? 1 : 37)) {
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(i * 7 + n);
    const std::size_t len = b.build(p, frame.data());
    ASSERT_TRUE(udp_checksum_ok(frame.data(), len)) << n;
    ASSERT_EQ(inet_checksum({frame.data() + 14, 20}), 0) << "IPv4 header checksum, n=" << n;
  }
  // A sum that folds to 0 must be sent as 0xFFFF (0 = "no checksum").
  bool saw_ffff = false;
  std::vector<std::uint8_t> p(4, 0);
  for (unsigned v = 0; v <= 0xFFFF; ++v) {
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
    const std::size_t len = b.build(p, frame.data());
    const std::uint16_t c = get16(frame.data() + 40);
    ASSERT_NE(c, 0) << v;
    ASSERT_TRUE(udp_checksum_ok(frame.data(), len)) << v;
    saw_ffff = saw_ffff || c == 0xFFFF;
  }
  EXPECT_TRUE(saw_ffff) << "non-vacuity: some payload sums to zero";
}

TEST(XdpFrame, ArpLookupWantsACompleteEntryOnThatDevice) {
  const std::string path = ::testing::TempDir() + "adsb_arp_table.txt";
  std::FILE* f = std::fopen(path.c_str(), "w");
  ASSERT_NE(f, nullptr);
  std::fputs("IP address       HW type     Flags       HW address            Mask     Device\n"
             "10.11.0.2        0x1         0x0         00:00:00:00:00:00     *        veth0\n"
             "10.11.0.3        0x1         0x2         02:00:00:00:00:33     *        eth9\n"
             "10.11.0.3        0x1         0x2         02:00:00:00:00:03     *        veth0\n"
             "10.11.0.4        0x1         0x6         02:00:00:00:00:04     *        veth0\n",
             f);
  std::fclose(f);
  MacAddress m{};
  EXPECT_FALSE(arp_lookup("10.11.0.2", "veth0", m, path.c_str())) << "incomplete entry";
  ASSERT_TRUE(arp_lookup("10.11.0.3", "veth0", m, path.c_str()));
  EXPECT_EQ(to_string(m), "02:00:00:00:00:03") << "the entry on the device asked for";
  ASSERT_TRUE(arp_lookup("10.11.0.4", "veth0", m, path.c_str())) << "permanent entries count";
  EXPECT_EQ(to_string(m), "02:00:00:00:00:04");
  EXPECT_FALSE(arp_lookup("10.11.0.3", "veth1", m, path.c_str()));
  EXPECT_FALSE(arp_lookup("10.11.0.9", "veth0", m, path.c_str()));
  EXPECT_FALSE(arp_lookup("10.11.0.3", "veth0", m, "/nonexistent/arp"));
  std::remove(path.c_str());
}

TEST(XdpSender, ParsesModesAndTransportFlags) {
  XdpSender::Mode m{};
  EXPECT_TRUE(XdpSender::parse_mode("copy", m));
  EXPECT_EQ(m, XdpSender::Mode::kCopy);
  EXPECT_TRUE(XdpSender::parse_mode("zerocopy", m));
  EXPECT_TRUE(XdpSender::parse_mode("auto", m));
  EXPECT_FALSE(XdpSender::parse_mode("zc", m));
  Transport t{};
  TxOptions o;
  EXPECT_TRUE(apply_tx_flag("--transport", "xdp", t, o));
  EXPECT_EQ(t, Transport::kXdp);
  EXPECT_TRUE(apply_tx_flag("--xdp-mode", "copy", t, o));
  EXPECT_EQ(o.xdp.mode, XdpSender::Mode::kCopy);
  EXPECT_TRUE(apply_tx_flag("--xdp-frames", "256", t, o));
  EXPECT_EQ(o.xdp.frames, 256u);
  EXPECT_TRUE(apply_tx_flag("--xdp-dst-mac", "02:00:00:00:00:02", t, o));
  EXPECT_FALSE(apply_tx_flag("--xdp-dst-mac", "nope", t, o));
  EXPECT_FALSE(apply_tx_flag("--xdp-frames", "12x", t, o));
  EXPECT_FALSE(apply_tx_flag("--transport", "dpdk", t, o));
  EXPECT_TRUE(is_tx_flag("--xdp-queue"));
  EXPECT_FALSE(is_tx_flag("--interface"));
}

// ------------------------------------------------------------------ live

struct Pair {
  std::string tx, rx;
};

// The veth pair from ADSB_XDP_TEST, or skip.
#define PAIR_OR_SKIP(pair)                                                                 \
  Pair pair;                                                                               \
  do {                                                                                     \
    if (!XdpSender::built()) GTEST_SKIP() << "built without AF_XDP";                      \
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

XdpSender::Options options(const Pair& p, XdpSender::Mode mode = XdpSender::Mode::kCopy) {
  XdpSender::Options o;
  o.udp.interface_host = p.tx;
  o.mode = mode;
  return o;
}

// Sends kN datagrams of varying length in windows and checks each arrived
// intact, once, in order.
void send_and_check(XdpSender& tx, Receiver& rx, std::uint64_t kN) {
  for (std::uint64_t s = 1; s <= kN; ++s) {
    ASSERT_TRUE(tx.send(datagram(s, 16 + s % 1400)));
    if (s % 64 == 0) {
      ASSERT_TRUE(rx.wait_count(s)) << "lost before " << s;
    }
  }
  ASSERT_TRUE(tx.drain());
  const auto got = rx.drain();
  ASSERT_EQ(got.size(), kN);
  for (std::uint64_t s = 1; s <= kN; ++s) {
    ASSERT_EQ(got[s - 1], datagram(s, 16 + s % 1400)) << s;
  }
  tx.refresh_kernel_stats();
  const auto& st = tx.stats();
  EXPECT_EQ(st.queued, kN);
  EXPECT_EQ(st.completed, kN);
  EXPECT_EQ(st.kick_errors, 0u) << "last errno " << st.last_error;
  EXPECT_EQ(st.tx_invalid, 0u);
}

TEST(XdpSender, MulticastArrivesIntactAndInOrder) {
  PAIR_OR_SKIP(p);
  Receiver rx(Receiver::Adopt{}, open_feed_receiver(Endpoint{"239.19.0.77", 0}, p.rx, 8 << 20));
  ASSERT_TRUE(rx.ok());
  XdpSender tx;
  std::string err;
  ASSERT_TRUE(tx.open(Endpoint{"239.19.0.77", rx.port}, options(p), &err)) << err;
  EXPECT_EQ(tx.bound_mode(), XdpSender::Mode::kCopy);
  EXPECT_EQ(tx.max_payload(), tx.device().mtu - 28u);
  send_and_check(tx, rx, 3000);
}

TEST(XdpSender, UnicastFindsTheMacInTheArpTable) {
  PAIR_OR_SKIP(p);
  Receiver rx(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  ASSERT_TRUE(rx.ok());
  XdpSender tx;
  std::string err;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, rx.port}, options(p), &err)) << err;
  send_and_check(tx, rx, 3000);
}

TEST(XdpSender, UnicastWithAnExplicitMac) {
  PAIR_OR_SKIP(p);
  InterfaceInfo far;
  ASSERT_TRUE(interface_for_address(p.rx, far));
  Receiver rx(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  XdpSender tx;
  auto o = options(p, XdpSender::Mode::kAuto);
  o.dst_mac = to_string(far.mac);
  std::string err;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, rx.port}, o, &err)) << err;
  send_and_check(tx, rx, 1000);
}

TEST(XdpSender, TwoFramesStillDeliverEverythingInOrder) {
  PAIR_OR_SKIP(p);
  Receiver rx(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  XdpSender tx;
  auto o = options(p);
  o.frames = 2;  // send() constantly waits for a frame to come back
  std::string err;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, rx.port}, o, &err)) << err;
  send_and_check(tx, rx, 5000);
}

TEST(XdpSender, RejectsWhatItCannotSend) {
  PAIR_OR_SKIP(p);
  Receiver rx(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  XdpSender tx;
  std::string err;
  EXPECT_FALSE(tx.send(datagram(1, 100))) << "not open";
  auto o = options(p);
  o.frames = 3;
  EXPECT_FALSE(tx.open(Endpoint{p.rx, rx.port}, o, &err));
  o = options(p);
  o.udp.interface_host = "0.0.0.0";
  EXPECT_FALSE(tx.open(Endpoint{p.rx, rx.port}, o, &err));
  EXPECT_FALSE(tx.open(Endpoint{"192.0.2.1", rx.port}, options(p), &err)) << "no ARP entry";
  EXPECT_NE(err.find("ARP"), std::string::npos) << err;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, rx.port}, options(p), &err)) << err;
  EXPECT_FALSE(tx.send(std::vector<std::uint8_t>(tx.max_payload() + 1)));
  EXPECT_FALSE(tx.send({}));
  EXPECT_TRUE(tx.send(datagram(1, tx.max_payload())));
  EXPECT_TRUE(tx.drain());
  const auto got = rx.drain();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], datagram(1, tx.max_payload()));
}

TEST(XdpSender, FeedTxSendsThroughIt) {
  PAIR_OR_SKIP(p);
  Receiver rx(Receiver::Adopt{}, open_udp(p.rx, 0, 8 << 20));
  FeedTx tx;
  UdpSender::Options udp;
  udp.interface_host = p.tx;
  TxOptions o;
  o.xdp.mode = XdpSender::Mode::kCopy;
  std::string err;
  ASSERT_TRUE(tx.open(Endpoint{p.rx, rx.port}, Transport::kXdp, udp, o, &err)) << err;
  EXPECT_TRUE(tx.send(datagram(9, 300)));
  EXPECT_TRUE(tx.drain());
  const std::string rep = tx.report();
  EXPECT_NE(rep.find("copy mode: 1 queued, 1 completed, 0 invalid"), std::string::npos) << rep;
  const auto got = rx.drain();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], datagram(9, 300));
}

}  // namespace
