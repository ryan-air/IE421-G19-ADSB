#include "adsb/ingest/beast_tcp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include "adsb/common/cache.hpp"

namespace adsb::ingest::detail {

namespace {

bool set_nonblocking(int fd, bool on) noexcept {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}

std::string describe(const std::string& host, std::uint16_t port, const char* what, int err) {
  return "connect " + host + ":" + std::to_string(port) + ": " + what +
         (err ? std::string(": ") + std::strerror(err) : std::string());
}

}  // namespace

int tcp_connect(const std::string& host, std::uint16_t port, std::uint32_t timeout_ms,
                int rcvbuf, bool nonblocking, std::string* error) noexcept {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  if (const int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res); rc != 0) {
    if (error) *error = "resolve " + host + ": " + gai_strerror(rc);
    return -1;
  }

  int fd = -1;
  int last_err = 0;
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      last_err = errno;
      continue;
    }
    if (rcvbuf > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    // Connect with a timeout: non-blocking connect, then poll for writability.
    set_nonblocking(fd, true);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      const int ready = ::poll(&p, 1, static_cast<int>(timeout_ms));
      if (ready == 1) {
        int so_err = 0;
        socklen_t len = sizeof so_err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
        rc = so_err == 0 ? 0 : -1;
        errno = so_err;
      } else {
        rc = -1;
        if (ready == 0) errno = ETIMEDOUT;  // otherwise poll() set errno
      }
    }
    if (rc == 0) {
      set_nonblocking(fd, nonblocking);
      freeaddrinfo(res);
      return fd;
    }
    last_err = errno;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (error) *error = describe(host, port, "failed", last_err);
  return -1;
}

long tcp_read(int fd, std::uint8_t* buf, std::size_t n, bool busy_poll) noexcept {
  if (busy_poll) {
    const ssize_t r = ::recv(fd, buf, n, 0);
    if (r > 0) return static_cast<long>(r);
    if (r == 0) return -1;  // orderly shutdown by the peer
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      cpu_relax();
      return 0;
    }
    return -1;
  }
  pollfd p{fd, POLLIN, 0};
  const int rc = ::poll(&p, 1, 100);  // wake regularly so `stop` is honoured
  if (rc == 0) return 0;
  if (rc < 0) return errno == EINTR ? 0 : -1;
  const ssize_t r = ::recv(fd, buf, n, 0);
  if (r > 0) return static_cast<long>(r);
  if (r == 0) return -1;
  return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

void tcp_close(int fd) noexcept { ::close(fd); }

void sleep_interruptible(std::uint32_t ms, const std::atomic<bool>& stop) noexcept {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace adsb::ingest::detail
