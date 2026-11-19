#pragma once

// Test helper: a UDP receiver on its own thread (feed transport tests).

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "adsb/feed/net.hpp"

namespace adsb::feed::testing {

using namespace std::chrono_literals;

// Reads on its own thread while the test sends, so a small kernel receive
// buffer (rmem_max caps SO_RCVBUF, about 200 KB in containers) does not drop
// datagrams. The thread owns `got_`; the test reads it only after join().
class Receiver {
 public:
  struct Adopt {};
  explicit Receiver(int rcvbuf = 8 << 20) : Receiver(Adopt{}, open_udp("127.0.0.1", 0, rcvbuf)) {}
  // Takes ownership of a bound, non-blocking UDP socket (or -1: !ok()).
  Receiver(Adopt, int fd) : fd_(fd) {
    port = fd_ >= 0 ? local_port(fd_) : 0;
    if (fd_ >= 0) start();
  }
  [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
  ~Receiver() {
    stop_and_join();
    close_fd(fd_);
  }
  // Waits until nothing has arrived for `quiet`, then returns what arrived
  // since the last drain (and keeps receiving).
  std::vector<std::vector<std::uint8_t>> drain(std::chrono::milliseconds quiet = 200ms) {
    std::uint64_t seen = count_.load();
    auto last = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - last < quiet) {
      std::this_thread::sleep_for(5ms);
      if (const std::uint64_t c = count_.load(); c != seen) {
        seen = c;
        last = std::chrono::steady_clock::now();
      }
    }
    stop_and_join();
    std::vector<std::vector<std::uint8_t>> out = std::move(got_);
    got_.clear();
    start();
    return out;
  }
  // Waits until `n` datagrams have arrived in total (or `timeout`). The tests
  // send in windows and wait, so a host stall cannot overflow the kernel
  // receive buffer: these tests are about correctness, not throughput.
  bool wait_count(std::uint64_t n, std::chrono::milliseconds timeout = 3s) const {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (count_.load() < n) {
      if (std::chrono::steady_clock::now() > end) return false;
      std::this_thread::yield();
    }
    return true;
  }
  std::uint16_t port = 0;

 private:
  void start() {
    stop_ = false;
    thread_ = std::thread([this] {
      std::uint8_t buf[4096];
      while (!stop_.load(std::memory_order_relaxed)) {
        const ssize_t n = ::recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
          got_.emplace_back(buf, buf + n);
          count_.fetch_add(1);
        }
      }
    });
  }
  void stop_and_join() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> count_{0};
  std::vector<std::vector<std::uint8_t>> got_;
  std::thread thread_;
};

}  // namespace adsb::feed::testing
