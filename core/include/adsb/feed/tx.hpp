#pragma once

// The publisher's UDP transport, chosen at startup (README_UPDATES.md §9.3:
// transports compared on identical histograms): kernel sendto(), or io_uring
// with or without a kernel submission thread. Satisfies the Publisher's
// `Out` requirement; the branch in send() is perfectly predictable.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "adsb/feed/net.hpp"
#include "adsb/feed/uring_sender.hpp"

namespace adsb::feed {

enum class Transport : std::uint8_t {
  kKernel,         // sendto() per datagram
  kUring,          // io_uring, SQPOLL: no syscall per datagram
  kUringNoSqpoll,  // io_uring, one io_uring_enter() per datagram
};

[[nodiscard]] inline bool parse_transport(std::string_view s, Transport& out) noexcept {
  if (s == "kernel") out = Transport::kKernel;
  else if (s == "uring") out = Transport::kUring;
  else if (s == "uring-nosqpoll") out = Transport::kUringNoSqpoll;
  else return false;
  return true;
}

[[nodiscard]] inline const char* transport_name(Transport t) noexcept {
  switch (t) {
    case Transport::kKernel: return "kernel";
    case Transport::kUring: return "uring";
    case Transport::kUringNoSqpoll: return "uring-nosqpoll";
  }
  return "?";
}

class FeedTx {
 public:
  [[nodiscard]] bool open(const Endpoint& dest, Transport t, const UdpSender::Options& udp,
                          UringSender::Options uring, std::string* error = nullptr) {
    transport_ = t;
    if (t == Transport::kKernel) return kernel_.open(dest, udp, error);
    uring.udp = udp;
    uring.sqpoll = t == Transport::kUring;
    return uring_.open(dest, uring, error);
  }
  bool send(std::span<const std::uint8_t> dgram) noexcept {
    return transport_ == Transport::kKernel ? kernel_.send(dgram) : uring_.send(dgram);
  }
  // Waits for datagrams still in flight (io_uring); true if none is left.
  bool drain(unsigned timeout_ms = 1000) noexcept {
    return transport_ == Transport::kKernel || uring_.drain(timeout_ms);
  }
  [[nodiscard]] Transport transport() const noexcept { return transport_; }
  [[nodiscard]] const UringSender::Stats& uring_stats() const noexcept { return uring_.stats(); }

  // One report line for the io_uring counters ("" for kernel UDP).
  [[nodiscard]] std::string report() const {
    if (transport_ == Transport::kKernel) return {};
    const auto& s = uring_.stats();
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    char buf[320];
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
};

}  // namespace adsb::feed
