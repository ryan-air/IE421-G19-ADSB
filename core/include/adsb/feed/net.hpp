#pragma once

// Socket helpers for the feed: UDP multicast/unicast send and receive, and
// TCP listeners. IPv4 only (multicast groups are IPv4 in this project).
// Every function reports failure through its return value and an optional
// error string; none throws.

#include <netinet/in.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace adsb::feed {

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
};

// Parses "HOST:PORT" (HOST may be a name or dotted quad). Port 0 is allowed.
[[nodiscard]] bool parse_endpoint(std::string_view text, Endpoint& out);
[[nodiscard]] std::string to_string(const Endpoint& e);

// Resolves to an IPv4 socket address.
[[nodiscard]] bool resolve_ipv4(const Endpoint& e, sockaddr_in& out, std::string* error = nullptr);
[[nodiscard]] bool is_multicast(const sockaddr_in& a) noexcept;
[[nodiscard]] bool same_address(const sockaddr_in& a, const sockaddr_in& b) noexcept;

// A UDP socket bound to `bind_host:port` (port 0 = ephemeral), non-blocking.
[[nodiscard]] int open_udp(const std::string& bind_host, std::uint16_t port, int rcvbuf,
                           std::string* error = nullptr);

// A non-blocking UDP socket receiving the feed at `feed`. If the address is a
// multicast group, the socket joins it on `interface_host` (an interface
// address; "0.0.0.0" lets the kernel choose) and allows other subscribers on
// the same host to bind the same port.
[[nodiscard]] int open_feed_receiver(const Endpoint& feed, const std::string& interface_host,
                                     int rcvbuf, std::string* error = nullptr);

// A non-blocking TCP listening socket.
[[nodiscard]] int open_tcp_listener(const std::string& bind_host, std::uint16_t port, int backlog,
                                    std::string* error = nullptr);

// The local port a socket is bound to (0 on error).
[[nodiscard]] std::uint16_t local_port(int fd) noexcept;

void close_fd(int fd) noexcept;

// Sends datagrams to one destination (the multicast group or a unicast
// address). Satisfies the Publisher's `Out` requirement.
class UdpSender {
 public:
  struct Options {
    std::string interface_host = "0.0.0.0";  // multicast egress interface
    int ttl = 1;                             // §9.2: never leaves the LAN
    bool loopback = true;                    // deliver to subscribers on this host
  };

  UdpSender() = default;
  ~UdpSender();
  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  [[nodiscard]] bool open(const Endpoint& dest, const Options& opt, std::string* error = nullptr);
  bool send(std::span<const std::uint8_t> dgram) noexcept;
  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  int fd_ = -1;
  sockaddr_in dest_{};
};

}  // namespace adsb::feed
