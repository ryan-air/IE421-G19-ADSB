#pragma once

// AF_XDP feed reception (README_UPDATES.md §9.3, subscriber side): an XDP
// program on the receiving device redirects the feed's UDP datagrams into an
// XSK socket, so they reach the subscriber without the kernel's IP/UDP stack
// (no skb, no socket queue, no recv() copy in zero-copy mode).
//
//   program: 27 eBPF instructions, built here and loaded with the raw
//            bpf() syscall (no clang BPF target, no libbpf, like the
//            publisher side). It looks at Ethernet + IPv4 (no options, not a
//            fragment) + UDP to the feed's address and port, and redirects
//            the frame to XSKMAP[rx queue]; everything else -- ARP, other
//            ports, the retransmit replies, IP options, fragments -- goes on
//            to the kernel (XDP_PASS). So does a feed frame that arrives on a
//            queue with no socket in the map (bpf_redirect_map's fallback).
//   attach:  a BPF link (kernel 5.9+), so the program detaches when the
//            receiver closes or the process dies; native (driver) mode if
//            the driver supports XDP, else generic (skb) mode.
//   checks:  receive() drops a redirected frame whose IPv4 header checksum
//            or lengths are wrong (as the kernel would); the UDP checksum is
//            not verified (frames sent from the same host can carry a
//            partial one) -- the Ethernet FCS covers the link.
//   size:    a frame must fit a UMEM frame after the kernel's headroom: open()
//            refuses a device whose MTU would not (jumbo frames), since a
//            redirected frame that does not fit is dropped, not passed on.
//   socket:  one XSK bound to one device queue, with a UMEM of `frames`
//            2 KiB frames that all start on the fill ring. receive() hands
//            each UDP payload to a callback and returns the frame to the
//            fill ring at once, so the ring never runs dry while the caller
//            keeps up. No allocation after open().
//
// One XSK per device queue and one XDP program per device: a host runs one
// AF_XDP feed handler per NIC (and fans out to local consumers through the
// shared-memory ring). While it runs, no other local socket on that device
// receives the feed group and port: the program takes those frames. A multi-queue NIC spreads flows over its queues: bind
// the queue the feed lands on (ethtool -N flow steering) or rely on the
// fallback, which passes feed frames on other queues to the kernel, where
// SubscriberClient's ordinary socket still receives them.
//
// Needs CAP_NET_ADMIN and CAP_BPF (load and attach) plus CAP_NET_RAW (the
// XSK). The UMEM (frames x 2 KiB) is pinned memory charged to the user's
// RLIMIT_MEMLOCK unless the process has CAP_IPC_LOCK. Linux only (ADSB_HAVE_AF_XDP); elsewhere open() fails with a message.
// The program builder and the frame parser are portable and tested
// everywhere (the tests run the program in a small eBPF interpreter).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adsb/feed/net.hpp"
#include "adsb/feed/xdp_sender.hpp"

namespace adsb::feed {

// One eBPF instruction, laid out like the kernel's struct bpf_insn on a
// little-endian host: regs = dst | src << 4 (the loader refuses to build on
// a big-endian host, where the nibbles swap).
struct BpfInsn {
  std::uint8_t code = 0;
  std::uint8_t regs = 0;
  std::int16_t off = 0;
  std::int32_t imm = 0;
};
static_assert(sizeof(BpfInsn) == 8, "struct bpf_insn layout");

inline constexpr std::int32_t kXdpPass = 2;
inline constexpr std::int32_t kXdpRedirect = 4;
inline constexpr std::int32_t kBpfFuncRedirectMap = 51;  // BPF_FUNC_redirect_map
inline constexpr std::uint8_t kBpfPseudoMapFd = 1;       // BPF_PSEUDO_MAP_FD
// Ethernet + IPv4 without options + UDP.
inline constexpr std::size_t kUdpFrameHeaderBytes = 14 + 20 + 8;

// The redirect program for UDP to dst_ip_be:dst_port (address in network
// byte order, 0 = any address; port in host order). `map_fd` is the XSKMAP
// (placed in the program's ld_imm64 as a pseudo map fd).
[[nodiscard]] std::vector<BpfInsn> xdp_feed_program(int map_fd, std::uint32_t dst_ip_be,
                                                    std::uint16_t dst_port);

// What udp_payload() found.
enum class FrameCheck : std::uint8_t {
  kOk,
  kShort,         // shorter than the headers
  kNotIpv4,       // not EtherType 0x0800, or not version 4
  kIpOptions,     // IHL != 5 (the program passes these to the kernel)
  kFragment,      // MF set or a fragment offset (likewise)
  kNotUdp,
  kWrongDest,     // another address or port
  kBadChecksum,   // IPv4 header checksum
  kBadLength,     // IP or UDP length disagrees with the frame
};
[[nodiscard]] const char* to_string(FrameCheck c) noexcept;

// Checks an Ethernet frame against the program's rules plus the lengths, and
// sets `payload` to the UDP payload. dst_ip_be 0 = any address.
[[nodiscard]] FrameCheck udp_payload(std::span<const std::uint8_t> frame, std::uint32_t dst_ip_be,
                                     std::uint16_t dst_port, std::span<const std::uint8_t>& payload) noexcept;

class XdpReceiver {
 public:
  using Mode = XdpSender::Mode;  // copy, zero-copy, or auto
  enum class Attach : std::uint8_t { kAuto, kDriver, kGeneric };
  struct Options {
    std::string interface_host;  // address of the receiving device (finds it)
    unsigned queue = 0;          // the device's RX queue to bind
    Mode mode = Mode::kAuto;
    Attach attach = Attach::kAuto;
    unsigned frames = 2048;      // UMEM frames (power of two, 64..65536)
  };
  struct Stats {
    std::uint64_t received = 0;    // feed payloads handed to the callback
    std::uint64_t rejected = 0;    // redirected frames that failed udp_payload (checksum, lengths: dropped)
    std::uint64_t wakeups = 0;     // fill-ring wakeups (need_wakeup mode)
    // From the kernel (refresh_kernel_stats):
    std::uint64_t rx_dropped = 0;           // no room in the RX ring, or other drops
    std::uint64_t rx_invalid = 0;           // invalid descriptors
    std::uint64_t rx_ring_full = 0;         // dropped because the RX ring was full
    std::uint64_t rx_fill_ring_empty = 0;   // the kernel found no frame to fill
  };
  // Called with each feed payload; the bytes are valid during the call only.
  using Callback = void (*)(void* ctx, std::span<const std::uint8_t> payload);

  XdpReceiver();
  ~XdpReceiver();  // detaches the program (closing the link) and the socket
  XdpReceiver(const XdpReceiver&) = delete;
  XdpReceiver& operator=(const XdpReceiver&) = delete;

  // True when this build has AF_XDP support (the kernel may still refuse).
  [[nodiscard]] static bool built() noexcept;
  [[nodiscard]] static bool parse_attach(std::string_view s, Attach& out) noexcept;
  [[nodiscard]] static const char* attach_name(Attach a) noexcept;

  // `feed`: the group (or local unicast address) and port the publisher
  // sends to; its address is what the program matches ("0.0.0.0" = any).
  [[nodiscard]] bool open(const Endpoint& feed, const Options& opt, std::string* error = nullptr);
  [[nodiscard]] bool is_open() const noexcept { return impl_ != nullptr; }
  // Pollable: POLLIN when the RX ring holds frames (and polling wakes the
  // kernel to refill, in need_wakeup mode).
  [[nodiscard]] int fd() const noexcept;
  // Hands up to `max` waiting payloads to `cb`; returns how many frames it
  // took off the RX ring (feed payloads plus rejected frames).
  std::size_t receive(Callback cb, void* ctx, std::size_t max) noexcept;
  template <class Fn>
  std::size_t receive(Fn& fn, std::size_t max) noexcept {
    return receive([](void* c, std::span<const std::uint8_t> p) { (*static_cast<Fn*>(c))(p); }, &fn, max);
  }
  // Reads the kernel's counters into stats() (a syscall: not for the hot path).
  void refresh_kernel_stats() noexcept;
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  [[nodiscard]] Mode bound_mode() const noexcept;     // copy or zero-copy, once open
  [[nodiscard]] Attach attach_mode() const noexcept;  // driver or generic, once open
  [[nodiscard]] const InterfaceInfo& device() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Stats stats_;
};

}  // namespace adsb::feed
