#include "adsb/feed/net.hpp"

#include "adsb/common/clock.hpp"
#include "adsb/feed/publisher.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>

namespace adsb::feed {

namespace {

void set_error(std::string* error, const std::string& what, int err) {
  if (error) *error = what + (err ? std::string(": ") + std::strerror(err) : std::string());
}

bool set_nonblocking(int fd) noexcept {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_reuse(int fd) noexcept {
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
}

// Opens and binds; returns -1 with `error` set on failure.
int bind_socket(int type, const std::string& host, std::uint16_t port, bool reuse, int rcvbuf,
                std::string* error) {
  sockaddr_in a{};
  if (!resolve_ipv4(Endpoint{host, port}, a, error)) return -1;
  const int fd = ::socket(AF_INET, type, 0);
  if (fd < 0) {
    set_error(error, "socket", errno);
    return -1;
  }
  if (reuse) set_reuse(fd);
  if (rcvbuf > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
#if defined(SO_NOSIGPIPE)
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
    set_error(error, "bind " + host + ":" + std::to_string(port), errno);
    ::close(fd);
    return -1;
  }
  if (!set_nonblocking(fd)) {
    set_error(error, "fcntl", errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

bool parse_endpoint(std::string_view text, Endpoint& out) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return false;
  unsigned port = 0;
  const char* b = text.data() + colon + 1;
  const char* e = text.data() + text.size();
  const auto r = std::from_chars(b, e, port);
  if (r.ec != std::errc() || r.ptr != e || port > 65535) return false;
  out.host = std::string(text.substr(0, colon));
  out.port = static_cast<std::uint16_t>(port);
  return true;
}

std::string to_string(const Endpoint& e) { return e.host + ":" + std::to_string(e.port); }

protocol::Session unique_session(char prefix) {
  static std::atomic<std::uint64_t> counter{0};
  std::uint64_t x = wall_ns() / 1000;
  x ^= static_cast<std::uint64_t>(::getpid()) << 40;
  x += counter.fetch_add(1) * 0x9E3779B97F4A7C15ULL >> 20;
  protocol::Session s{};
  s[0] = prefix;
  const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (std::size_t i = s.size() - 1; i >= 1; --i) {
    s[i] = digits[x % 36];
    x /= 36;
  }
  return s;
}

bool resolve_ipv4(const Endpoint& e, sockaddr_in& out, std::string* error) {
  out = sockaddr_in{};
  out.sin_family = AF_INET;
  out.sin_port = htons(e.port);
  if (inet_pton(AF_INET, e.host.c_str(), &out.sin_addr) == 1) return true;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* res = nullptr;
  if (const int rc = getaddrinfo(e.host.c_str(), nullptr, &hints, &res); rc != 0 || res == nullptr) {
    if (error) *error = "resolve " + e.host + ": " + gai_strerror(rc);
    return false;
  }
  out.sin_addr = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return true;
}

bool is_multicast(const sockaddr_in& a) noexcept {
  return (ntohl(a.sin_addr.s_addr) >> 28) == 0xE;  // 224.0.0.0/4
}

bool same_address(const sockaddr_in& a, const sockaddr_in& b) noexcept {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

int open_udp(const std::string& bind_host, std::uint16_t port, int rcvbuf, std::string* error) {
  return bind_socket(SOCK_DGRAM, bind_host, port, false, rcvbuf, error);
}

int open_feed_receiver(const Endpoint& feed, const std::string& interface_host, int rcvbuf,
                       std::string* error) {
  sockaddr_in group{};
  if (!resolve_ipv4(feed, group, error)) return -1;
  if (!is_multicast(group)) return bind_socket(SOCK_DGRAM, feed.host, feed.port, false, rcvbuf, error);

  // Bind to the group address itself, so this socket only receives that
  // group (binding 0.0.0.0 would also deliver other groups on the same port).
  const int fd = bind_socket(SOCK_DGRAM, feed.host, feed.port, true, rcvbuf, error);
  if (fd < 0) return -1;
#if defined(IP_MULTICAST_ALL)
  const int zero = 0;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero);
#endif
  sockaddr_in iface{};
  if (!resolve_ipv4(Endpoint{interface_host, 0}, iface, error)) {
    ::close(fd);
    return -1;
  }
  ip_mreq mreq{};
  mreq.imr_multiaddr = group.sin_addr;
  mreq.imr_interface = iface.sin_addr;
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
    set_error(error, "join " + feed.host + " on " + interface_host, errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

int open_tcp_listener(const std::string& bind_host, std::uint16_t port, int backlog,
                      std::string* error) {
  const int fd = bind_socket(SOCK_STREAM, bind_host, port, true, 0, error);
  if (fd < 0) return -1;
  if (::listen(fd, backlog) != 0) {
    set_error(error, "listen", errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

std::uint16_t local_port(int fd) noexcept {
  sockaddr_in a{};
  socklen_t len = sizeof a;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
  return ntohs(a.sin_port);
}

void close_fd(int fd) noexcept {
  if (fd >= 0) ::close(fd);
}

UdpSender::~UdpSender() { close_fd(fd_); }

bool UdpSender::open(const Endpoint& dest, const Options& opt, std::string* error) {
  close_fd(fd_);
  fd_ = -1;
  if (!resolve_ipv4(dest, dest_, error)) return false;
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    set_error(error, "socket", errno);
    return false;
  }
  const int sndbuf = 4 << 20;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
  if (is_multicast(dest_)) {
    sockaddr_in iface{};
    if (!resolve_ipv4(Endpoint{opt.interface_host, 0}, iface, error)) {
      ::close(fd);
      return false;
    }
    const auto ttl = static_cast<unsigned char>(opt.ttl);
    const unsigned char loop = opt.loopback ? 1 : 0;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) != 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) != 0 ||
        (iface.sin_addr.s_addr != htonl(INADDR_ANY) &&
         setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface.sin_addr, sizeof iface.sin_addr) != 0)) {
      set_error(error, "multicast options", errno);
      ::close(fd);
      return false;
    }
  }
  fd_ = fd;
  return true;
}

bool UdpSender::send(std::span<const std::uint8_t> dgram) noexcept {
  const ssize_t n = ::sendto(fd_, dgram.data(), dgram.size(), 0,
                             reinterpret_cast<const sockaddr*>(&dest_), sizeof dest_);
  return n == static_cast<ssize_t>(dgram.size());
}

}  // namespace adsb::feed
