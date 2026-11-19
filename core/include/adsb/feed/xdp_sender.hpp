#pragma once

// AF_XDP feed transport (README_UPDATES.md §9.3): the publisher writes whole
// Ethernet + IPv4 + UDP frames into a UMEM shared with the kernel and queues
// them on an XSK socket's TX ring, bypassing the kernel's IP/UDP stack.
//
//   send(): take a free UMEM frame, write the headers (prebuilt template;
//           IPv4 and UDP checksums computed per frame) and the datagram,
//           queue its descriptor, kick the socket (sendto with no data), and
//           take back the frames the completion ring returns. Never blocks
//           unless every frame is in flight (then it waits for one:
//           backpressure). No allocation after open().
//   modes:  copy (any driver, including veth: the kernel copies each frame
//           into an skb and hands it to the driver), zero-copy (the NIC DMAs
//           straight from the UMEM: needs driver support, e.g. Intel igc /
//           i40e / ice, Mellanox mlx5), or auto (zero-copy if the driver can,
//           else copy). The report says which one the kernel granted.
//
// Order: the TX ring is FIFO and the kernel transmits in ring order; a
// descriptor it cannot send yet (device busy, TX batch limit) stays queued
// and goes out on the next kick, so datagrams never overtake each other.
// What the driver or the peer drops after that is a loss the subscriber sees
// as a gap, which retransmit recovers.
//
// Addressing: `udp.interface_host` must be the address of the sending device
// (that is how the device is found); it is the source IP. The destination MAC
// is the multicast MAC (01:00:5e + low 23 bits of the group) for a group,
// else `dst_mac`, else the device's ARP entry for the destination. Frames
// leave through the device only: unlike kernel UDP there is no local
// loopback copy, so subscribers on the publishing host receive nothing
// (they use the shared-memory ring). Needs CAP_NET_RAW.
//
// Publisher side only: subscribers receive with recv(). The subscriber side
// of the plan (an XDP program redirecting the feed port into an XSK) is not
// built. Linux only (ADSB_HAVE_AF_XDP); elsewhere open() fails with a
// message. The frame builder below is portable and tested everywhere.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "adsb/feed/net.hpp"

namespace adsb::feed {

using MacAddress = std::array<std::uint8_t, 6>;

// "aa:bb:cc:dd:ee:ff" (case-insensitive).
[[nodiscard]] bool parse_mac(std::string_view text, MacAddress& out) noexcept;
[[nodiscard]] std::string to_string(const MacAddress& m);
// RFC 1112: 01:00:5e followed by the low 23 bits of the group address.
// `group_be` is in network byte order (as in sockaddr_in).
[[nodiscard]] MacAddress multicast_mac(std::uint32_t group_be) noexcept;

// Internet checksum (RFC 1071) of `data`, starting from the partial sum
// `sum` (host order, not yet folded). Returns the folded, complemented value
// in host order; a buffer that includes a correct checksum sums to 0.
[[nodiscard]] std::uint16_t inet_checksum(std::span<const std::uint8_t> data,
                                          std::uint32_t sum = 0) noexcept;

// Ethernet II + IPv4 (no options, DF, id 0) + UDP headers for one flow.
class UdpFrameBuilder {
 public:
  static constexpr std::size_t kHeaderBytes = 14 + 20 + 8;

  // Addresses in network byte order, ports in host order.
  void init(const MacAddress& src_mac, const MacAddress& dst_mac, std::uint32_t src_ip_be,
            std::uint32_t dst_ip_be, std::uint16_t src_port, std::uint16_t dst_port,
            std::uint8_t ttl) noexcept;
  // Writes headers + payload into `out` (at least kHeaderBytes + payload
  // bytes) and returns the frame length.
  std::size_t build(std::span<const std::uint8_t> payload, std::uint8_t* out) const noexcept;

 private:
  std::array<std::uint8_t, kHeaderBytes> tmpl_{};
  std::uint32_t pseudo_sum_ = 0;  // UDP pseudo-header without the length
};

// The device that owns an IPv4 address (Linux; elsewhere returns false).
struct InterfaceInfo {
  std::string name;
  unsigned index = 0;
  MacAddress mac{};
  unsigned mtu = 0;
};
[[nodiscard]] bool interface_for_address(const std::string& ipv4, InterfaceInfo& out,
                                         std::string* error = nullptr);
// The complete ARP entry for `ipv4` on `device`, from a table in the format
// of Linux /proc/net/arp (incomplete entries do not count).
[[nodiscard]] bool arp_lookup(const std::string& ipv4, const std::string& device, MacAddress& out,
                              const char* table = "/proc/net/arp");

class XdpSender {
 public:
  enum class Mode : std::uint8_t { kAuto, kCopy, kZeroCopy };
  struct Options {
    UdpSender::Options udp;       // interface_host = the sending device's address; ttl
    Mode mode = Mode::kAuto;
    unsigned queue = 0;           // the device's TX queue to bind
    unsigned frames = 1024;       // UMEM frames (power of two) = datagrams in flight at most
    std::string dst_mac;          // unicast destination MAC; empty = ARP table
    std::uint16_t src_port = 0;   // 0 = the destination port
  };
  struct Stats {
    std::uint64_t queued = 0;       // datagrams put on the TX ring
    std::uint64_t completed = 0;    // frames the kernel handed back (sent or dropped by the driver)
    std::uint64_t kicks = 0;        // sendto() wakeups
    std::uint64_t kick_retries = 0; // kicks that left descriptors queued (EAGAIN/EBUSY/ENOBUFS)
    std::uint64_t kick_errors = 0;  // other kick errors (see last_error)
    std::uint64_t frame_waits = 0;  // send() had to wait for a free frame
    std::uint64_t tx_invalid = 0;   // descriptors the kernel rejected (XDP_STATISTICS)
    int last_error = 0;
  };
  static constexpr std::size_t kFrameBytes = 2048;

  XdpSender();
  ~XdpSender();  // waits (briefly) for frames in flight, then closes
  XdpSender(const XdpSender&) = delete;
  XdpSender& operator=(const XdpSender&) = delete;

  // True when this build has AF_XDP support (the kernel may still refuse).
  [[nodiscard]] static bool built() noexcept;
  [[nodiscard]] static bool parse_mode(std::string_view s, Mode& out) noexcept;
  [[nodiscard]] static const char* mode_name(Mode m) noexcept;

  [[nodiscard]] bool open(const Endpoint& dest, const Options& opt, std::string* error = nullptr);
  bool send(std::span<const std::uint8_t> dgram) noexcept;
  // Kicks until every queued frame has completed (or timeout_ms passed).
  bool drain(unsigned timeout_ms = 1000) noexcept;
  // Reads the kernel's counters into stats() (a syscall: not for the hot path).
  void refresh_kernel_stats() noexcept;
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  // What the kernel granted (copy or zero-copy) once open.
  [[nodiscard]] Mode bound_mode() const noexcept;
  [[nodiscard]] const InterfaceInfo& device() const noexcept;
  [[nodiscard]] std::size_t max_payload() const noexcept;

 private:
  void reap() noexcept;
  void kick() noexcept;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  Stats stats_;
};

}  // namespace adsb::feed
