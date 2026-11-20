#pragma once

// The publisher's UDP transport, chosen at startup (README_UPDATES.md §9.3:
// transports compared on identical histograms): kernel sendto(), io_uring
// with or without a kernel submission thread, or AF_XDP. Satisfies the
// Publisher's `Out` requirement; the branch in send() is perfectly
// predictable.

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "adsb/feed/net.hpp"
#include "adsb/feed/uring_sender.hpp"
#include "adsb/feed/xdp_sender.hpp"

namespace adsb::feed {

enum class Transport : std::uint8_t {
  kKernel,         // sendto() per datagram
  kUring,          // io_uring, SQPOLL: no syscall per datagram
  kUringNoSqpoll,  // io_uring, one io_uring_enter() per datagram
  kXdp,            // AF_XDP: own Ethernet/IPv4/UDP frames on an XSK TX ring
};

[[nodiscard]] inline bool parse_transport(std::string_view s, Transport& out) noexcept {
  if (s == "kernel") out = Transport::kKernel;
  else if (s == "uring") out = Transport::kUring;
  else if (s == "uring-nosqpoll") out = Transport::kUringNoSqpoll;
  else if (s == "xdp") out = Transport::kXdp;
  else return false;
  return true;
}

[[nodiscard]] inline const char* transport_name(Transport t) noexcept {
  switch (t) {
    case Transport::kKernel: return "kernel";
    case Transport::kUring: return "uring";
    case Transport::kUringNoSqpoll: return "uring-nosqpoll";
    case Transport::kXdp: return "xdp";
  }
  return "?";
}

// Per-transport settings (UdpSender::Options apply to every transport).
struct TxOptions {
  UringSender::Options uring;
  XdpSender::Options xdp;
};

// Command-line flags shared by the publishing tools (a macro so it can join
// the tools' usage string literals).
#define ADSB_TX_OPTIONS_HELP \
    "  --transport T           feed UDP transport: kernel (sendto, default), uring (io_uring with a\n" \
    "                          kernel submission thread: no syscall per datagram), uring-nosqpoll,\n" \
    "                          xdp (AF_XDP: our own Ethernet/IP/UDP frames; needs --interface ADDR of\n" \
    "                          the sending device and CAP_NET_RAW; no copy to local subscribers)\n" \
    "  --uring-sq-cpu N        pin io_uring's submission thread to CPU N (Linux)\n" \
    "  --uring-in-flight N     io_uring sends in flight at most (buffer pool, default 64)\n" \
    "  --xdp-mode M            auto (default: zero-copy if the driver can), copy, zerocopy\n" \
    "  --xdp-queue N           the device's TX queue (default 0)\n" \
    "  --xdp-frames N          UMEM frames = datagrams in flight at most (power of two, default 1024)\n" \
    "  --xdp-dst-mac MAC       unicast destination MAC (default: the ARP table)\n"

// True if `flag` is one of the flags above; then `value` is its argument.
[[nodiscard]] inline bool is_tx_flag(std::string_view flag) noexcept {
  return flag == "--transport" || flag == "--uring-sq-cpu" || flag == "--uring-in-flight" ||
         flag == "--xdp-mode" || flag == "--xdp-queue" || flag == "--xdp-frames" || flag == "--xdp-dst-mac";
}
[[nodiscard]] inline bool apply_tx_flag(std::string_view flag, const std::string& value, Transport& t,
                                        TxOptions& o) noexcept {
  auto num = [&](unsigned& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || *end != '\0' || v > 0xFFFFFFFFul) return false;
    out = static_cast<unsigned>(v);
    return true;
  };
  if (flag == "--transport") return parse_transport(value, t);
  if (flag == "--uring-sq-cpu") {
    o.uring.sq_cpu = std::atoi(value.c_str());
    return true;
  }
  if (flag == "--uring-in-flight") return num(o.uring.max_in_flight);
  if (flag == "--xdp-mode") return XdpSender::parse_mode(value, o.xdp.mode);
  if (flag == "--xdp-queue") return num(o.xdp.queue);
  if (flag == "--xdp-frames") return num(o.xdp.frames);
  if (flag == "--xdp-dst-mac") {
    MacAddress m{};
    o.xdp.dst_mac = value;
    return parse_mac(value, m);
  }
  return false;
}

class FeedTx {
 public:
  [[nodiscard]] bool open(const Endpoint& dest, Transport t, const UdpSender::Options& udp, TxOptions opt,
                          std::string* error = nullptr) {
    transport_ = t;
    switch (t) {
      case Transport::kKernel: return kernel_.open(dest, udp, error);
      case Transport::kUring:
      case Transport::kUringNoSqpoll:
        opt.uring.udp = udp;
        opt.uring.sqpoll = t == Transport::kUring;
        return uring_.open(dest, opt.uring, error);
      case Transport::kXdp:
        opt.xdp.udp = udp;
        return xdp_.open(dest, opt.xdp, error);
    }
    return false;
  }
  bool send(std::span<const std::uint8_t> dgram) noexcept {
    switch (transport_) {
      case Transport::kKernel: return kernel_.send(dgram);
      case Transport::kXdp: return xdp_.send(dgram);
      default: return uring_.send(dgram);
    }
  }
  // Waits for datagrams still in flight (io_uring, AF_XDP); true if none is left.
  bool drain(unsigned timeout_ms = 1000) noexcept {
    switch (transport_) {
      case Transport::kKernel: return true;
      case Transport::kXdp: return xdp_.drain(timeout_ms);
      default: return uring_.drain(timeout_ms);
    }
  }
  [[nodiscard]] Transport transport() const noexcept { return transport_; }
  [[nodiscard]] const UringSender::Stats& uring_stats() const noexcept { return uring_.stats(); }
  [[nodiscard]] XdpSender& xdp() noexcept { return xdp_; }

  // One report line for the transport's counters ("" for kernel UDP).
  [[nodiscard]] std::string report() {
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    char buf[384];
    if (transport_ == Transport::kKernel) return {};
    if (transport_ == Transport::kXdp) {
      xdp_.refresh_kernel_stats();
      const auto& s = xdp_.stats();
      std::snprintf(buf, sizeof buf,
                    "af_xdp      %s, %s mode: %llu queued, %llu completed, %llu invalid, "
                    "%llu kicks (%llu retried, %llu failed, last errno %d), %llu frame waits",
                    xdp_.device().name.c_str(), XdpSender::mode_name(xdp_.bound_mode()), u(s.queued),
                    u(s.completed), u(s.tx_invalid), u(s.kicks), u(s.kick_retries), u(s.kick_errors),
                    s.last_error, u(s.frame_waits));
      return buf;
    }
    const auto& s = uring_.stats();
    std::snprintf(buf, sizeof buf,
                  "io_uring    %s: %llu queued, %llu completed, %llu failed (last errno %d), %llu reordered, "
                  "%llu buffer waits, %llu SQ full, %llu SQ thread wakeups, %llu submit syscalls",
                  transport_name(transport_), u(s.queued), u(s.completed), u(s.failed), s.last_error,
                  u(s.reordered), u(s.buffer_waits), u(s.sq_full), u(s.wakeups), u(s.submit_calls));
    return buf;
  }

 private:
  Transport transport_ = Transport::kKernel;
  UdpSender kernel_;
  UringSender uring_;
  XdpSender xdp_;
};

}  // namespace adsb::feed
