#pragma once

// Live Beast ingest over TCP: connects to a readsb / dump1090 Beast output
// port (30005 by default), parses the byte stream, and delivers each frame as
// a record::Record stamped with its receive time (README_UPDATES.md §5.3: the
// Beast source is kept alongside the future in-house demodulator).
//
//  * Receive time (`rx_ns`) is mono_ns() taken right after recv() returns, the
//    pipeline's event-time domain. All frames from one recv() share it.
//  * The radio timestamp (`radio_ts`) is the Beast 12 MHz counter.
//  * Disconnects are expected (readsb restarts, network blips): the source
//    discards any partial frame, backs off exponentially, and reconnects.
//  * Two receive modes: blocking poll() with a short timeout (default; gentle on
//    laptops) or busy polling a non-blocking socket (lowest latency; uses a
//    core). Either way `stop` is checked at least every ~100 ms.
//
// The run loop is single-threaded and allocation-free after connect; the sink
// is called synchronously on the ingest thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "adsb/common/clock.hpp"
#include "adsb/ingest/beast.hpp"
#include "adsb/record/recording.hpp"

namespace adsb::ingest {

struct BeastTcpConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 30005;
  std::uint8_t line = 0;               // receiver line id stamped on every record
  bool busy_poll = false;              // spin on a non-blocking socket instead of poll()
  int receive_buffer_bytes = 4 << 20;  // SO_RCVBUF request (the kernel may cap it)
  std::uint32_t reconnect_initial_ms = 100;
  std::uint32_t reconnect_max_ms = 5000;
  std::uint32_t connect_timeout_ms = 2000;
};

struct BeastTcpStats {
  std::uint64_t bytes = 0;
  std::uint64_t frames = 0;
  std::uint64_t connects = 0;          // successful connections
  std::uint64_t disconnects = 0;       // connections lost after being established
  std::uint64_t connect_failures = 0;  // attempts that did not connect
  BeastStats parser{};                 // cumulative across connections
};

namespace detail {

// Socket helpers (beast_tcp.cpp). All return -1 on failure.
int tcp_connect(const std::string& host, std::uint16_t port, std::uint32_t timeout_ms,
                int rcvbuf, bool nonblocking, std::string* error) noexcept;
// > 0: bytes read. 0: nothing yet (timeout / EAGAIN). -1: connection closed or failed.
long tcp_read(int fd, std::uint8_t* buf, std::size_t n, bool busy_poll) noexcept;
void tcp_close(int fd) noexcept;
// Sleeps up to `ms`, returning early once `stop` is set.
void sleep_interruptible(std::uint32_t ms, const std::atomic<bool>& stop) noexcept;

}  // namespace detail

class BeastTcpSource {
 public:
  explicit BeastTcpSource(BeastTcpConfig cfg) : cfg_(std::move(cfg)) {}
  BeastTcpSource(const BeastTcpSource&) = delete;
  BeastTcpSource& operator=(const BeastTcpSource&) = delete;
  ~BeastTcpSource() { close(); }

  // Connects (and reconnects) until `stop` is set; calls
  // sink(const record::Record&) for every frame. `on_status(const char*)` is
  // called on connect / disconnect / failure for logging (may be a no-op).
  template <typename Sink, typename Status>
  void run(const std::atomic<bool>& stop, Sink&& sink, Status&& on_status) {
    std::uint32_t backoff = cfg_.reconnect_initial_ms;
    while (!stop.load(std::memory_order_relaxed)) {
      std::string err;
      fd_ = detail::tcp_connect(cfg_.host, cfg_.port, cfg_.connect_timeout_ms,
                                cfg_.receive_buffer_bytes, cfg_.busy_poll, &err);
      if (fd_ < 0) {
        ++stats_.connect_failures;
        on_status(err.c_str());
        detail::sleep_interruptible(backoff, stop);
        backoff = backoff * 2 > cfg_.reconnect_max_ms ? cfg_.reconnect_max_ms : backoff * 2;
        continue;
      }
      ++stats_.connects;
      backoff = cfg_.reconnect_initial_ms;
      on_status("connected");
      parser_.reset();

      while (!stop.load(std::memory_order_relaxed)) {
        const long n = detail::tcp_read(fd_, buf_, sizeof buf_, cfg_.busy_poll);
        if (n == 0) continue;
        if (n < 0) {
          ++stats_.disconnects;
          on_status("disconnected");
          break;
        }
        const std::uint64_t rx = mono_ns();
        stats_.bytes += static_cast<std::uint64_t>(n);
        stats_.frames += parser_.feed(buf_, static_cast<std::size_t>(n), [&](const BeastFrame& f) {
          record::Record r;
          r.rx_ns = rx;
          r.radio_ts = f.timestamp;
          r.line = cfg_.line;
          r.type = static_cast<std::uint8_t>(f.type);
          r.signal = f.signal;
          r.len = f.len;
          std::memcpy(r.payload, f.payload, f.len);
          sink(static_cast<const record::Record&>(r));
        });
      }
      close();
      // A partial frame from the lost connection must not merge with the next one.
      parser_.reset();
      if (!stop.load(std::memory_order_relaxed)) detail::sleep_interruptible(backoff, stop);
    }
    close();
  }

  template <typename Sink>
  void run(const std::atomic<bool>& stop, Sink&& sink) {
    run(stop, std::forward<Sink>(sink), [](const char*) {});
  }

  // Stats are written by the ingest thread; read them after run() returns.
  [[nodiscard]] BeastTcpStats stats() const noexcept {
    BeastTcpStats s = stats_;
    s.parser = parser_.stats();
    return s;
  }
  [[nodiscard]] const BeastTcpConfig& config() const noexcept { return cfg_; }

 private:
  void close() noexcept {
    if (fd_ >= 0) detail::tcp_close(fd_);
    fd_ = -1;
  }

  BeastTcpConfig cfg_;
  BeastParser parser_;
  BeastTcpStats stats_{};
  int fd_ = -1;
  std::uint8_t buf_[64 * 1024];
};

}  // namespace adsb::ingest
