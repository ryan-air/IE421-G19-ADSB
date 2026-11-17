#pragma once

// Feed publisher (README_UPDATES.md §9.1–9.2, protocol/feed_spec.md §2).
//
// The publisher is the track engine's sink: every TrackUpdate / TrackDelete
// gets the next sequence number, is packed into a MoldUDP64-style datagram,
// and is sent on the incremental feed (multicast or unicast UDP). It also
// hands a copy of each sequenced message to the recovery side through a
// lock-free SPSC log, so retransmission and snapshots never touch the hot
// thread:
//
//   engine -> Publisher -> Out::send()      (hot thread: no allocation, no locks)
//                \-> MessageLog (SPSC) -> RecoveryServer (own thread)
//
// Flush policy (part of the benchmark, §9.2):
//   * immediate (max_batch_delay_ns == 0): one datagram per message;
//   * batch: a datagram is sent when the next message would not fit or when
//     the oldest pending message is max_batch_delay_ns old (checked on each
//     publish and in poll()).
// Heartbeats: when nothing has been sent for heartbeat_interval_ns, poll()
// sends a heartbeat (count 0, seq = next sequence number), so subscribers
// detect a lost tail even when traffic stops (§9.5 item 3). After
// end_session(), poll() repeats the end-of-session marker at the same
// interval instead: the marker cannot be retransmitted, so a subscriber that
// lost every copy would otherwise never learn the session ended.
//
// Timers use the injected clock (mono_ns by default). They only pace
// transmission; message contents depend on event time alone, apart from the
// optional publish_ts_ns stamp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "adsb/common/cache.hpp"
#include "adsb/common/clock.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/common/spsc_ring.hpp"
#include "adsb/protocol/moldudp64.hpp"

namespace adsb::feed {

// Largest message the recovery log, retransmit store and subscriber buffer
// hold. TrackUpdate (64 B) is the largest message in feed v0.
inline constexpr std::size_t kMaxMessageBytes = 64;

// One sequenced message, as handed to the recovery thread.
struct LogEntry {
  std::uint64_t seq = 0;
  std::uint16_t len = 0;
  std::uint8_t bytes[kMaxMessageBytes] = {};
};

inline constexpr std::size_t kMessageLogCapacity = 1u << 16;
using MessageLog = SpscRing<LogEntry, kMessageLogCapacity>;

// A session id that differs on every start (feed_spec §2): `prefix` plus nine
// base-36 digits from the wall clock in microseconds mixed with the process
// id, so two starts in the same second (a supervisor restart loop) still
// differ. A subscriber cannot detect a reused id: its book would silently
// diverge. Ids given by hand must be unique for the same reason.
[[nodiscard]] protocol::Session unique_session(char prefix);

[[nodiscard]] inline std::unique_ptr<MessageLog> make_message_log() {
  return std::make_unique<MessageLog>();
}

struct PublisherConfig {
  protocol::Session session{};
  std::uint64_t first_seq = 1;                        // sequence numbers start at 1 (feed_spec §2)
  std::uint64_t max_batch_delay_ns = 0;               // 0 = immediate
  std::uint64_t heartbeat_interval_ns = 1'000'000'000;
  bool stamp_publish_ts = true;                       // TrackUpdate.publish_ts_ns = clock()
};

struct PublisherStats {
  std::uint64_t messages = 0;
  std::uint64_t packets = 0;      // data packets handed to Out::send
  std::uint64_t heartbeats = 0;
  std::uint64_t bytes = 0;
  std::uint64_t send_failures = 0;  // Out::send returned false
  std::uint64_t rejected = 0;       // message larger than kMaxMessageBytes
  std::uint64_t log_full_waits = 0; // recovery log was full; the publisher spun
};

// `Out` must provide `bool send(std::span<const std::uint8_t>) noexcept`.
template <typename Out>
class Publisher {
 public:
  using Clock = std::uint64_t (*)();

  Publisher(const PublisherConfig& cfg, Out& out, MessageLog* log = nullptr,
            Clock clock = &default_clock)
      : cfg_(cfg), out_(out), log_(log), clock_(clock), builder_(cfg.session, cfg.first_seq),
        last_send_ns_(clock()) {}

  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  // ---- Track engine sink --------------------------------------------------
  void on_update(const protocol::TrackUpdate& u) {
    std::uint8_t buf[protocol::TrackUpdate::kWireSize];
    protocol::TrackUpdate m = u;
    if (cfg_.stamp_publish_ts) m.publish_ts_ns = clock_();
    protocol::write(m, buf);
    publish(buf, static_cast<std::uint16_t>(sizeof buf));
  }
  void on_delete(const protocol::TrackDelete& d) {
    std::uint8_t buf[protocol::TrackDelete::kWireSize];
    protocol::write(d, buf);
    publish(buf, static_cast<std::uint16_t>(sizeof buf));
  }

  // Sequences and sends any message of up to kMaxMessageBytes.
  void publish(const std::uint8_t* msg, std::uint16_t len) {
    if (len == 0 || len > kMaxMessageBytes || ended_) {
      ++stats_.rejected;
      return;
    }
    if (!builder_.add(msg, len)) {
      flush();
      (void)builder_.add(msg, len);  // always fits in an empty packet
    }
    const std::uint64_t seq = builder_.next_seq() - 1;
    log_message(seq, msg, len);
    ++stats_.messages;
    if (cfg_.max_batch_delay_ns == 0) {
      flush();
      return;
    }
    const std::uint64_t now = clock_();
    if (builder_.count() == 1) batch_start_ns_ = now;
    if (now - batch_start_ns_ >= cfg_.max_batch_delay_ns) flush();
  }

  // Call regularly (e.g. whenever the hot loop is idle): sends a batch whose
  // delay has expired, and a heartbeat when the feed has been quiet.
  void poll() {
    const std::uint64_t now = clock_();
    if (ended_) {
      if (now - last_send_ns_ >= cfg_.heartbeat_interval_ns) send(builder_.heartbeat(true));
      return;
    }
    if (!builder_.empty()) {
      if (now - batch_start_ns_ >= cfg_.max_batch_delay_ns) flush();
      return;
    }
    if (now - last_send_ns_ >= cfg_.heartbeat_interval_ns) {
      send(builder_.heartbeat(false));
      ++stats_.heartbeats;
    }
  }

  // Sends any pending messages now.
  void flush() {
    if (builder_.empty()) return;
    send(builder_.finish());
    ++stats_.packets;
  }

  // Flushes and sends the end-of-session marker `copies` times; poll() keeps
  // repeating it afterwards. Nothing can be published after.
  void end_session(int copies = 3) {
    if (ended_) return;
    flush();
    for (int i = 0; i < copies; ++i) send(builder_.heartbeat(true));
    ended_ = true;
  }

  // Sequence number the next message will get.
  [[nodiscard]] std::uint64_t next_seq() const noexcept { return builder_.next_seq(); }
  [[nodiscard]] const PublisherStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const PublisherConfig& config() const noexcept { return cfg_; }

 private:
  static std::uint64_t default_clock() { return mono_ns(); }

  void send(std::span<const std::uint8_t> dgram) {
    if (!out_.send(dgram)) ++stats_.send_failures;
    stats_.bytes += dgram.size();
    last_send_ns_ = clock_();
  }

  void log_message(std::uint64_t seq, const std::uint8_t* msg, std::uint16_t len) {
    if (log_ == nullptr) return;
    LogEntry e;
    e.seq = seq;
    e.len = len;
    std::memcpy(e.bytes, msg, len);
    // The recovery thread drains the log every millisecond, so it is full only
    // if that thread is stalled. Waiting (rather than dropping) keeps the
    // retransmit store and snapshots exact; the wait is counted.
    if (!log_->try_push(e)) {
      ++stats_.log_full_waits;
      while (!log_->try_push(e)) cpu_relax();
    }
  }

  PublisherConfig cfg_;
  Out& out_;
  MessageLog* log_;
  Clock clock_;
  protocol::PacketBuilder builder_;
  std::uint64_t batch_start_ns_ = 0;
  std::uint64_t last_send_ns_;
  bool ended_ = false;
  PublisherStats stats_;
};

// ---------------------------------------------------------------------------
// Fault injection (§9.5 item 4): wraps an Out and drops datagrams.
// ---------------------------------------------------------------------------

struct LossConfig {
  double drop_rate = 0.0;      // probability that a drop burst starts at a datagram
  std::uint32_t drop_burst = 1;  // datagrams dropped per burst
  std::uint64_t seed = 1;
  std::uint64_t only_first = 0;  // inject only into the first N datagrams (0 = all)
};

// Deterministic for a given seed and datagram order.
class LossInjector {
 public:
  explicit LossInjector(const LossConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {}
  // True if this datagram should be dropped.
  [[nodiscard]] bool drop() noexcept {
    ++seen_;
    if (cfg_.only_first != 0 && seen_ > cfg_.only_first) return false;
    if (left_ > 0) {
      --left_;
      ++dropped_;
      return true;
    }
    if (cfg_.drop_rate > 0.0 && rng_.chance(cfg_.drop_rate)) {
      left_ = cfg_.drop_burst > 0 ? cfg_.drop_burst - 1 : 0;
      ++dropped_;
      return true;
    }
    return false;
  }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

 private:
  LossConfig cfg_;
  Rng rng_;
  std::uint32_t left_ = 0;
  std::uint64_t seen_ = 0;
  std::uint64_t dropped_ = 0;
};

template <typename Out>
class LossyOut {
 public:
  LossyOut(Out& inner, const LossConfig& cfg) : inner_(inner), loss_(cfg) {}
  bool send(std::span<const std::uint8_t> d) noexcept {
    if (loss_.drop()) return true;  // "sent" into the void
    return inner_.send(d);
  }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return loss_.dropped(); }

 private:
  Out& inner_;
  LossInjector loss_;
};

}  // namespace adsb::feed
