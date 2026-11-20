#pragma once

// io_uring feed transport (README_UPDATES.md §9.3): the publisher's datagrams
// go out through an io_uring instead of one sendto() per datagram.
//
//   send(): copy the datagram into a free buffer of a fixed pool, queue an
//           IORING_OP_SEND (MSG_DONTWAIT) on the registered, connected UDP
//           socket, collect any completions that are ready. Never blocks
//           unless every buffer is in flight (then it waits for one:
//           backpressure). No allocation after open().
//   SQPOLL: a kernel thread picks up submissions, so the hot thread makes no
//           system call per datagram (only to wake that thread after it went
//           idle). Without SQPOLL every send() is one io_uring_enter(), the
//           same count as sendto(), which is the fair comparison point.
//
// Errors of a datagram are known only when its completion arrives, so send()
// returns false only for what it can see itself (too large, not open); async
// failures are counted in stats() and reported by the tools.
//
// Order: the publisher's sequencing assumes datagrams leave in order. When a
// socket buffer is full, io_uring by default parks the request until the
// socket is writable (even on an O_NONBLOCK socket), and a later datagram can
// overtake it: the unit test with a full send buffer saw thousands of
// reorders that way. MSG_DONTWAIT marks every send no-wait instead, so a full
// buffer completes it with EAGAIN: the datagram is lost (counted in `failed`;
// retransmit recovers it), exactly like a failed non-blocking sendto(), and
// datagrams are issued in submission order. Completions arriving out of
// submission order are counted (`reordered`; the tests assert 0).
//
// Not zero-copy: IORING_OP_SEND_ZC costs more than a copy for datagrams this
// small (under 1.5 KB); registered buffers matter only for it.
//
// Linux only, built when liburing is found (ADSB_HAVE_IO_URING). Elsewhere
// open() fails with a message and the tools reject --transport uring.

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "adsb/feed/net.hpp"

namespace adsb::feed {

class UringSender {
 public:
  struct Options {
    UdpSender::Options udp;          // TTL, multicast loopback and interface
    bool sqpoll = true;              // kernel submission thread (no syscall per send)
    int sq_cpu = -1;                 // pin that thread (Linux CPU number), -1 = no pin
    unsigned sq_idle_ms = 1000;      // the thread sleeps after this long without work
    unsigned max_in_flight = 64;     // buffers in the pool = sends outstanding at most
  };
  struct Stats {
    std::uint64_t queued = 0;          // datagrams handed to the ring
    std::uint64_t completed = 0;       // completions collected
    std::uint64_t failed = 0;          // completions with an error or a short write
    std::uint64_t reordered = 0;       // completions that arrived out of submission order
    std::uint64_t buffer_waits = 0;    // send() had to wait for a free buffer
    std::uint64_t sq_full = 0;         // submission queue full (SQ thread behind)
    std::uint64_t wakeups = 0;         // SQPOLL thread was asleep and had to be woken (a syscall)
    std::uint64_t submit_calls = 0;    // io_uring_enter calls made by send() (no SQPOLL)
    int last_error = 0;                // errno of the last failed completion
  };
  static constexpr std::size_t kBufferBytes = 2048;  // > any feed datagram (MTU-sized)

  UringSender();
  ~UringSender();  // waits (briefly) for writes in flight, then closes
  UringSender(const UringSender&) = delete;
  UringSender& operator=(const UringSender&) = delete;

  // True when this build has io_uring support (the kernel may still refuse).
  [[nodiscard]] static bool built() noexcept;

  [[nodiscard]] bool open(const Endpoint& dest, const Options& opt, std::string* error = nullptr);
  // Sends on an already connected datagram socket of any family (takes
  // ownership; made non-blocking). opt.udp is ignored. Used by the tests to
  // reach a full send buffer, which loopback UDP never does.
  [[nodiscard]] bool open_connected(int fd, const Options& opt, std::string* error = nullptr);
  bool send(std::span<const std::uint8_t> dgram) noexcept;
  // Waits until every queued datagram has completed (or timeout_ms passed).
  // Returns true if none is left in flight.
  bool drain(unsigned timeout_ms = 1000) noexcept;
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  [[nodiscard]] int fd() const noexcept;

 private:
  // Collects every completion that is ready; with `wait`, blocks for one first.
  void reap(bool wait) noexcept;
  bool setup(int fd, const Options& opt, std::string* error);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  Stats stats_;
};

}  // namespace adsb::feed
