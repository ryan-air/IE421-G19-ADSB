#pragma once

// Recovery side of the feed (README_UPDATES.md §9.5, protocol/feed_spec.md §2–3):
// the retransmit server and the snapshot server, fed by the publisher's
// MessageLog and running on their own thread.
//
//   RetransmitStore  ring of the last K sequenced messages (K = 65,536 by default)
//   RecoveryState    the pure logic: drains the log into the store and a
//                    FeedBook, answers retransmit requests, builds snapshots.
//                    No sockets, so tests can drive it directly.
//   RecoveryServer   RecoveryState + a unicast UDP socket for retransmit
//                    requests + a TCP listener for snapshots, in one poll loop.
//
// Retransmit replies are ordinary data packets of the same session, sent to
// the requester's address. A request the server can no longer serve (its
// first sequence number has left the ring) gets a header-only packet whose
// `seq` is the oldest sequence number still held: the "NAK" of the TLA+ model
// (spec/FeedRecovery.tla, ServeRequest). The subscriber then takes a snapshot.

#include <netinet/in.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "adsb/feed/book.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/snapshot.hpp"
#include "adsb/protocol/moldudp64.hpp"

namespace adsb::feed {

class RetransmitStore {
 public:
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 24;  // 16 Mi messages, ~1.3 GB
  // `capacity` is rounded up to a power of two, at most kMaxCapacity.
  explicit RetransmitStore(std::size_t capacity = 1u << 16, std::uint64_t first_seq = 1);

  // Appends the message with sequence number next(). Returns false (and
  // stores nothing) for any other seq or an oversized message.
  bool add(std::uint64_t seq, std::span<const std::uint8_t> msg);

  // Holds [oldest(), next()).
  [[nodiscard]] std::uint64_t oldest() const noexcept;
  [[nodiscard]] std::uint64_t next() const noexcept { return next_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

  // nullptr unless oldest() <= seq < next().
  [[nodiscard]] const LogEntry* get(std::uint64_t seq) const noexcept;

 private:
  std::vector<LogEntry> slots_;
  std::uint64_t mask_;
  std::uint64_t first_;
  std::uint64_t next_;
};

struct RecoveryConfig {
  protocol::Session session{};
  std::uint64_t first_seq = 1;
  std::size_t ring_capacity = 1u << 16;      // retransmit window (messages)
  std::uint32_t max_reply_messages = 4096;   // a larger request is truncated; the subscriber asks again
};

struct RecoveryStats {
  std::uint64_t logged = 0;            // messages drained from the publisher
  std::uint64_t log_errors = 0;        // out-of-sequence log entries (a bug if nonzero)
  std::uint64_t requests = 0;
  std::uint64_t malformed_requests = 0;
  std::uint64_t wrong_session = 0;     // request for another session: ignored
  std::uint64_t naks = 0;
  std::uint64_t served_messages = 0;
  std::uint64_t reply_packets = 0;
  std::uint64_t unavailable = 0;       // request beyond the last logged message: nothing sent
  std::uint64_t snapshots = 0;
};

class RecoveryState {
 public:
  explicit RecoveryState(const RecoveryConfig& cfg);

  // Moves every message currently in the log into the store and the book.
  std::size_t drain(MessageLog& log);
  // Same, for one message (tests and in-process use).
  void on_message(std::uint64_t seq, std::span<const std::uint8_t> msg);

  // Handles one request datagram. `reply(span)` is called for each reply
  // datagram: data packets, or one NAK.
  template <typename Reply>
  void handle_request(std::span<const std::uint8_t> dgram, Reply&& reply);

  // The snapshot stream as of the last logged message.
  void make_snapshot(std::vector<std::uint8_t>& out);

  [[nodiscard]] const FeedBook& book() const noexcept { return book_; }
  [[nodiscard]] const RetransmitStore& store() const noexcept { return store_; }
  [[nodiscard]] std::uint64_t last_seq() const noexcept { return store_.next() - 1; }
  [[nodiscard]] const RecoveryStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const RecoveryConfig& config() const noexcept { return cfg_; }

 private:
  RecoveryConfig cfg_;
  RetransmitStore store_;
  FeedBook book_;
  std::uint64_t last_event_ts_ = 0;
  RecoveryStats stats_;
};

template <typename Reply>
void RecoveryState::handle_request(std::span<const std::uint8_t> dgram, Reply&& reply) {
  ++stats_.requests;
  if (dgram.size() != protocol::RetransmitRequest::kWireSize) {
    ++stats_.malformed_requests;
    return;
  }
  protocol::RetransmitRequest req;
  protocol::read(dgram.data(), req);
  if (req.session != cfg_.session) {
    ++stats_.wrong_session;
    return;
  }
  if (req.count == 0 || req.seq == 0) {
    ++stats_.malformed_requests;
    return;
  }
  if (req.seq < store_.oldest()) {
    ++stats_.naks;
    protocol::PacketBuilder nak(cfg_.session, store_.oldest());
    reply(nak.heartbeat(false));
    return;
  }
  if (req.seq >= store_.next()) {  // not logged yet (or never published)
    ++stats_.unavailable;
    return;
  }
  std::uint64_t end = req.seq + req.count;
  if (end > store_.next()) end = store_.next();
  if (end - req.seq > cfg_.max_reply_messages) end = req.seq + cfg_.max_reply_messages;
  protocol::PacketBuilder b(cfg_.session, req.seq);
  for (std::uint64_t s = req.seq; s < end; ++s) {
    const LogEntry* e = store_.get(s);
    if (e == nullptr) break;  // cannot happen: [req.seq, end) is inside the store
    if (!b.add(e->bytes, e->len)) {
      reply(b.finish());
      ++stats_.reply_packets;
      (void)b.add(e->bytes, e->len);
    }
    ++stats_.served_messages;
  }
  if (b.empty()) return;  // never send an empty data packet: it would read as a NAK
  reply(b.finish());
  ++stats_.reply_packets;
}

// ---------------------------------------------------------------------------

struct RecoveryServerConfig {
  RecoveryConfig recovery;
  std::string bind_host = "0.0.0.0";
  std::uint16_t retransmit_port = 0;  // 0 = ephemeral (see retransmit_port())
  std::uint16_t snapshot_port = 0;
  LossConfig reply_loss;              // fault injection on retransmit replies
  int max_snapshot_clients = 16;
  std::uint32_t snapshot_client_timeout_ms = 10'000;
};

// Live counters, readable from any thread.
struct RecoveryServerCounters {
  std::atomic<std::uint64_t> logged{0};
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> naks{0};
  std::atomic<std::uint64_t> served_messages{0};
  std::atomic<std::uint64_t> replies_dropped{0};  // by fault injection
  std::atomic<std::uint64_t> snapshots{0};
  std::atomic<std::uint64_t> snapshot_clients_dropped{0};
};

class RecoveryServer {
 public:
  RecoveryServer(const RecoveryServerConfig& cfg, MessageLog& log);
  ~RecoveryServer();
  RecoveryServer(const RecoveryServer&) = delete;
  RecoveryServer& operator=(const RecoveryServer&) = delete;

  // Binds both sockets. Call before run().
  [[nodiscard]] bool open(std::string* error = nullptr);
  [[nodiscard]] std::uint16_t retransmit_port() const noexcept { return rtx_port_; }
  [[nodiscard]] std::uint16_t snapshot_port() const noexcept { return snap_port_; }

  // The server loop; returns when `stop` becomes true. Drains the log at
  // least every millisecond.
  void run(const std::atomic<bool>& stop);

  // Safe to read after run() returns.
  [[nodiscard]] const RecoveryState& state() const noexcept { return state_; }
  [[nodiscard]] const RecoveryServerCounters& counters() const noexcept { return counters_; }

 private:
  struct Client {
    int fd = -1;
    std::vector<std::uint8_t> buf;
    std::size_t off = 0;
    std::uint64_t start_ns = 0;
  };
  void drain();
  void serve_requests();
  void accept_clients();
  void write_clients(const std::vector<short>& revents, std::size_t first_index);

  RecoveryServerConfig cfg_;
  MessageLog& log_;
  RecoveryState state_;
  LossInjector reply_loss_;
  int rtx_fd_ = -1;
  int listen_fd_ = -1;
  std::uint16_t rtx_port_ = 0;
  std::uint16_t snap_port_ = 0;
  std::vector<Client> clients_;
  RecoveryServerCounters counters_;
};

}  // namespace adsb::feed
