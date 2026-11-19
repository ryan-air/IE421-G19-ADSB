#pragma once

// Feed subscriber (README_UPDATES.md §9.5–9.6, protocol/feed_spec.md §4).
//
// SubscriberCore is the recovery protocol of spec/FeedRecovery.tla as code,
// with no sockets and no clock of its own (the caller passes `now`), so it can
// be driven deterministically by tests and simulations:
//
//   TLA+ action   here
//   -----------   ----------------------------------------------------------
//   RecvMcast     on_datagram(kFeed): apply in order, buffer what is ahead
//   SendRequest   maybe_request(): request [expected, first buffered) when a
//                 gap is known and no request is outstanding (or it timed out)
//   RecvReply     on_datagram(kRetransmit): data is accepted like multicast;
//                 a NAK (header-only reply, seq = oldest held) that shows the
//                 gap is gone switches to snapshot mode
//   Snapshot      on_snapshot(): adopt the state as of S, drop buffered
//                 messages <= S, apply the contiguous rest
//
// Beyond the model: sessions. A packet from an unknown session means the
// publisher restarted: the book is cleared and, unless the packet starts at
// sequence 1 (state "as of 0" is empty), a snapshot is taken. A change of
// session at sequence 1 is confirmed with the snapshot server in the
// background (confirming()), while the subscriber stays live: a late packet
// of the old session can arrive after the new session's first packets, and
// opaque ids cannot tell which is newer; the server, the live publisher,
// can. Late packets from sessions already left are dropped.
//
// SubscriberClient wraps the core with real sockets: the feed (multicast or
// unicast UDP), a unicast UDP socket for retransmit requests and replies, and
// a blocking TCP fetch for snapshots (packets keep queueing in the kernel's
// receive buffer meanwhile).

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "adsb/feed/book.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/snapshot.hpp"
#include "adsb/protocol/moldudp64.hpp"

namespace adsb::feed {

struct SubscriberConfig {
  static constexpr std::uint32_t kMaxWindow = 1u << 20;  // ~84 MB of buffer
  std::uint32_t window = 1u << 14;                   // messages buffered ahead of `expected` (rounded up to a power of two, at most kMaxWindow)
  std::uint16_t max_request = 1024;                  // messages per retransmit request (at most `window`)
  std::uint64_t retransmit_timeout_ns = 10'000'000;  // re-request after this long without progress
  std::uint64_t snapshot_retry_ns = 500'000'000;     // retry a failed snapshot after this long
};

struct SubscriberStats {
  std::uint64_t feed_packets = 0;
  std::uint64_t retransmit_packets = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t malformed = 0;
  std::uint64_t applied = 0;            // messages applied in sequence order
  std::uint64_t duplicates = 0;         // messages already applied
  std::uint64_t buffered = 0;           // messages that arrived ahead of `expected`
  std::uint64_t window_overflow = 0;    // too far ahead to buffer (recovered later)
  std::uint64_t oversize = 0;           // could not buffer: longer than kMaxMessageBytes
  std::uint64_t gaps = 0;               // times a new gap was detected
  std::uint64_t requests = 0;
  std::uint64_t request_retries = 0;    // re-sent after the timeout
  std::uint64_t truncated_replies = 0;  // answer ended short of the request: the rest asked at once
  std::uint64_t naks = 0;
  std::uint64_t snapshot_requests = 0;
  std::uint64_t snapshots_loaded = 0;
  std::uint64_t sessions_confirmed = 0; // the server's snapshot agreed with a session change
  std::uint64_t snapshot_failures = 0;
  std::uint64_t session_changes = 0;
  std::uint64_t stale_packets = 0;      // previous session, or a reply for another session
  std::uint64_t unknown_messages = 0;
};

// Where the core sends retransmit requests.
class SubscriberIo {
 public:
  virtual ~SubscriberIo() = default;
  virtual void send_request(const protocol::RetransmitRequest& req) = 0;
};

// Optional application callbacks. The book is already updated when they run.
class SubscriberHandler {
 public:
  virtual ~SubscriberHandler() = default;
  virtual void on_message(std::uint64_t /*seq*/, std::span<const std::uint8_t> /*msg*/) {}
  virtual void on_reset() {}           // new session: the book was cleared
  virtual void on_snapshot_loaded() {} // the book was replaced from a snapshot
  virtual void on_tick(std::uint64_t /*now_ns*/) {}  // SubscriberClient::run: about every millisecond
};

class SubscriberCore {
 public:
  enum class Source : std::uint8_t { kFeed, kRetransmit };
  enum class Mode : std::uint8_t { kLive, kSnapshot };

  explicit SubscriberCore(const SubscriberConfig& cfg = {}, SubscriberIo* io = nullptr,
                          SubscriberHandler* handler = nullptr);

  void set_io(SubscriberIo* io) noexcept { io_ = io; }
  void set_handler(SubscriberHandler* h) noexcept { handler_ = h; }
  [[nodiscard]] SubscriberHandler* handler() const noexcept { return handler_; }

  void on_datagram(std::span<const std::uint8_t> dgram, Source src, std::uint64_t now_ns);
  // Timers: re-sends a request that made no progress.
  void poll(std::uint64_t now_ns);

  // True when the caller should fetch a snapshot now (then call on_snapshot
  // or on_snapshot_failed). Rate-limited by snapshot_retry_ns.
  [[nodiscard]] bool take_snapshot_request(std::uint64_t now_ns);
  void on_snapshot(const SnapshotData& snap, std::uint64_t now_ns);
  void on_snapshot_failed(std::uint64_t now_ns);

  [[nodiscard]] const FeedBook& book() const noexcept { return book_; }
  [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
  // One past the highest sequence number known to have been published.
  [[nodiscard]] std::uint64_t known_next() const noexcept { return known_next_; }
  [[nodiscard]] Mode mode() const noexcept { return mode_; }
  [[nodiscard]] bool has_session() const noexcept { return has_session_; }
  [[nodiscard]] const protocol::Session& session() const noexcept { return session_; }
  // End of session seen, and every message before it applied.
  [[nodiscard]] bool ended() const noexcept {
    return eos_ && mode_ == Mode::kLive && expected_ >= eos_seq_;
  }
  [[nodiscard]] bool request_outstanding() const noexcept { return req_active_; }
  // A change of session waits for the snapshot server's confirmation
  // (take_snapshot_request() asks for it; the subscriber stays live).
  [[nodiscard]] bool confirming() const noexcept { return confirm_; }
  [[nodiscard]] const SubscriberStats& stats() const noexcept { return stats_; }

 private:
  struct Slot {
    std::uint64_t seq = 0;  // 0 = empty (sequence numbers start at 1)
    std::uint16_t len = 0;
    std::uint8_t bytes[kMaxMessageBytes] = {};
  };

  void new_session(const protocol::Session& s);
  void accept(std::uint64_t seq, std::span<const std::uint8_t> msg);
  void apply(std::uint64_t seq, std::span<const std::uint8_t> msg);
  void drain_pending();
  void maybe_request(std::uint64_t now_ns);
  void enter_snapshot();

  SubscriberConfig cfg_;
  SubscriberIo* io_;
  SubscriberHandler* handler_;
  FeedBook book_;
  std::vector<Slot> pending_;
  std::uint64_t mask_;

  protocol::Session session_{};
  bool has_session_ = false;
  // Sessions we have left (most recent kRetiredSessions). A packet from any
  // of them is stale; remembering only one would let a very late packet from
  // two sessions ago switch the subscriber back.
  static constexpr std::size_t kRetiredSessions = 8;
  std::array<protocol::Session, kRetiredSessions> retired_{};
  std::size_t retired_count_ = 0;
  std::size_t retired_next_ = 0;
  [[nodiscard]] bool is_retired(const protocol::Session& s) const noexcept;
  void retire(const protocol::Session& s) noexcept;
  void unretire(const protocol::Session& s) noexcept;

  Mode mode_ = Mode::kLive;
  std::uint64_t expected_ = 1;
  std::uint64_t known_next_ = 1;
  bool eos_ = false;
  std::uint64_t eos_seq_ = 0;

  bool req_active_ = false;
  std::uint64_t req_from_ = 0;  // first message of the outstanding request
  std::uint64_t req_to_ = 0;
  std::uint64_t req_sent_ns_ = 0;

  bool gap_open_ = false;

  bool snap_due_ = false;              // fetch as soon as possible
  bool confirm_ = false;               // live, but the session change awaits the server's word
  std::uint64_t snap_last_ns_ = 0;     // time of the last fetch attempt
  std::uint64_t snap_anchor_ = 0;      // first seq buffered in snapshot mode (0 = none yet)
  protocol::Session snap_req_session_{};  // session we were in when the last fetch was requested

  SubscriberStats stats_;
};

// ---------------------------------------------------------------------------

struct SubscriberClientConfig {
  Endpoint feed{"239.19.0.1", 41900};
  std::string interface_host = "0.0.0.0";  // multicast interface to join on
  Endpoint retransmit{"127.0.0.1", 41901};
  Endpoint snapshot{"127.0.0.1", 41902};
  int rcvbuf = 8 << 20;
  std::uint32_t snapshot_timeout_ms = 3000;
  LossConfig feed_loss;  // fault injection on received feed datagrams
  SubscriberConfig core;
};

class SubscriberClient : private SubscriberIo {
 public:
  explicit SubscriberClient(const SubscriberClientConfig& cfg);
  ~SubscriberClient() override;
  SubscriberClient(const SubscriberClient&) = delete;
  SubscriberClient& operator=(const SubscriberClient&) = delete;

  [[nodiscard]] bool open(std::string* error = nullptr);

  // Receives and recovers until `stop` is set, or (if stop_at_end) the
  // session ended and everything was recovered. Application callbacks go
  // through core().set_handler().
  void run(const std::atomic<bool>& stop, bool stop_at_end = false);

  // The port the feed socket is bound to (useful when the config said 0).
  [[nodiscard]] std::uint16_t feed_port() const noexcept { return local_port(feed_fd_); }
  [[nodiscard]] SubscriberCore& core() noexcept { return core_; }
  [[nodiscard]] const SubscriberCore& core() const noexcept { return core_; }
  [[nodiscard]] std::uint64_t feed_dropped() const noexcept { return loss_.dropped(); }
  [[nodiscard]] const std::string& last_snapshot_error() const noexcept { return snap_error_; }

  // Fetches one snapshot over TCP (blocking, with timeouts).
  [[nodiscard]] static bool fetch_snapshot(const Endpoint& server, std::uint32_t timeout_ms,
                                           SnapshotData& out, std::string* error);

 private:
  void send_request(const protocol::RetransmitRequest& req) override;
  void handle_snapshot(std::uint64_t now);

  SubscriberClientConfig cfg_;
  SubscriberCore core_;
  LossInjector loss_;
  int feed_fd_ = -1;
  int req_fd_ = -1;
  sockaddr_in rtx_addr_{};
  std::string snap_error_;
};

}  // namespace adsb::feed
