#pragma once

// ShmSubscriber: a same-host feed subscriber on the shared-memory ring
// (shm_ring.hpp), the counterpart of SubscriberClient for UDP.
//
// It keeps the same FeedBook and calls the same SubscriberHandler as the UDP
// subscriber, so applications do not care which transport they are on. There
// is no retransmission: the ring itself is the retransmit buffer. Recovery is:
//
//   join       if the whole session is still in the ring, read it from its
//              first message; otherwise load a snapshot (TCP snapshot server)
//              and continue from its as_of_seq + 1
//   lapped     the writer overwrote a message before we read it: load a
//              snapshot and continue after it (README_UPDATES.md §9.3)
//   replaced   a new publisher replaced the ring (new session): reopen it,
//              clear the book, and join again
//   ended      the publisher marked the end of the session and every message
//              before it has been applied
//   gave up    lapped after the end of the session, and no snapshot could be
//              had for give_up_after_end_ns (the publisher is gone): reported,
//              so a caller waiting for the end does not wait forever
//
// A snapshot is usable only if it belongs to the ring's session, the ring
// behind the name is still the one we read (a restarted publisher that reused
// the session id would otherwise pair its snapshot with the old ring's
// messages), and the ring still holds the message right after it; otherwise
// it is retried.

#include <atomic>
#include <cstdint>
#include <string>

#include "adsb/feed/book.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/shm_ring.hpp"
#include "adsb/feed/subscriber.hpp"

namespace adsb::feed {

struct ShmSubscriberConfig {
  std::string name = "/adsb-feed";
  Endpoint snapshot{"127.0.0.1", 41902};
  std::uint32_t snapshot_timeout_ms = 3000;
  std::uint64_t snapshot_retry_ns = 100'000'000;  // between failed snapshot attempts
  std::uint64_t replace_check_ns = 200'000'000;   // how often an idle reader checks for a new ring
  std::uint64_t reopen_retry_ns = 1'000'000;      // while no ring exists under the name (shm_open is cheap)
  std::uint64_t give_up_after_end_ns = 5'000'000'000;  // see "gave up" above
};

struct ShmSubscriberStats {
  std::uint64_t applied = 0;           // messages applied in sequence order
  std::uint64_t laps = 0;              // times the writer overtook us
  std::uint64_t snapshots_loaded = 0;
  std::uint64_t snapshot_failures = 0; // fetch failed, wrong session, or too old for the ring
  std::uint64_t rings_opened = 0;      // first open plus reopens after a replacement
  std::uint64_t unknown_messages = 0;
};

class ShmSubscriber {
 public:
  enum class State : std::uint8_t { kNoRing, kNeedSync, kLive, kGaveUp };

  explicit ShmSubscriber(const ShmSubscriberConfig& cfg, SubscriberHandler* handler = nullptr);

  // Maps the ring. Returns false if no valid ring exists yet (run() keeps trying).
  [[nodiscard]] bool open(std::string* error = nullptr);

  // Applies up to `max` available messages; recovers if needed. Returns the
  // number applied (0 = nothing new). `now_ns` is mono_ns().
  std::size_t poll(std::uint64_t now_ns, std::size_t max = 4096);

  // Polls until `stop`, or (stop_at_end) until the session has ended and been
  // applied. Spins between polls when idle (lowest latency, uses a core),
  // unless idle_sleep_us > 0.
  void run(const std::atomic<bool>& stop, bool stop_at_end = false, unsigned idle_sleep_us = 0);

  [[nodiscard]] const FeedBook& book() const noexcept { return book_; }
  [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
  [[nodiscard]] State state() const noexcept { return state_; }
  // mono_ns() when the subscriber last became live (joined or resynced). A
  // message published before this was read as catch-up, not live.
  [[nodiscard]] std::uint64_t live_since_ns() const noexcept { return live_since_ns_; }
  [[nodiscard]] bool ended() const noexcept;
  // Lapped after the end of the session and no snapshot was available in time.
  [[nodiscard]] bool gave_up() const noexcept { return state_ == State::kGaveUp; }
  [[nodiscard]] const ShmSubscriberStats& stats() const noexcept { return stats_; }
  [[nodiscard]] protocol::Session session() const noexcept { return session_; }
  [[nodiscard]] const std::string& last_error() const noexcept { return error_; }
  void set_handler(SubscriberHandler* h) noexcept { handler_ = h; }

 private:
  void sync(std::uint64_t now_ns);
  void reset_book();
  void check_replaced(std::uint64_t now_ns);

  ShmSubscriberConfig cfg_;
  SubscriberHandler* handler_;
  ShmRegion region_;
  RingView ring_;
  FeedBook book_;
  State state_ = State::kNoRing;
  std::uint64_t expected_ = 0;
  protocol::Session session_{};
  std::uint64_t instance_ = 0;
  std::uint64_t last_snapshot_try_ns_ = 0;
  std::uint64_t last_replace_check_ns_ = 0;
  std::uint64_t last_open_try_ns_ = 0;
  std::uint64_t live_since_ns_ = 0;
  std::uint64_t ended_unsynced_since_ns_ = 0;  // when we first needed a snapshot after the end
  bool tried_snapshot_ = false;
  std::string error_;
  ShmSubscriberStats stats_;
};

}  // namespace adsb::feed
