#include "adsb/feed/subscriber.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "adsb/common/clock.hpp"
#include "adsb/ingest/beast_tcp.hpp"

namespace adsb::feed {

using protocol::PacketKind;

namespace {

std::uint32_t round_up_pow2(std::uint32_t n) {
  if (n > SubscriberConfig::kMaxWindow) n = SubscriberConfig::kMaxWindow;
  std::uint32_t p = 2;
  while (p < n) p <<= 1;
  return p;
}

// Largest jump ahead of `expected` a packet may claim. Anything beyond is
// treated as malformed (a forged or corrupt header would otherwise create a
// gap that can never be filled, or wrap seq + count).
constexpr std::uint64_t kMaxSeqJump = 1ULL << 32;

}  // namespace

// ---------------------------------------------------------------------------
// SubscriberCore
// ---------------------------------------------------------------------------

SubscriberCore::SubscriberCore(const SubscriberConfig& cfg, SubscriberIo* io,
                               SubscriberHandler* handler)
    : cfg_(cfg), io_(io), handler_(handler) {
  cfg_.window = round_up_pow2(cfg_.window);
  if (cfg_.max_request == 0) cfg_.max_request = 1;
  pending_.resize(cfg_.window);
  mask_ = cfg_.window - 1;
}

bool SubscriberCore::is_retired(const protocol::Session& s) const noexcept {
  for (std::size_t i = 0; i < retired_count_; ++i) {
    if (retired_[i] == s) return true;
  }
  return false;
}

void SubscriberCore::retire(const protocol::Session& s) noexcept {
  if (is_retired(s)) return;
  retired_[retired_next_] = s;
  retired_next_ = (retired_next_ + 1) % kRetiredSessions;
  if (retired_count_ < kRetiredSessions) ++retired_count_;
}

void SubscriberCore::unretire(const protocol::Session& s) noexcept {
  for (std::size_t i = 0; i < retired_count_; ++i) {
    if (retired_[i] == s) retired_[i] = protocol::Session{};  // (an all-NUL id is never used)
  }
}

void SubscriberCore::new_session(const protocol::Session& s) {
  if (has_session_) {
    retire(session_);
    ++stats_.session_changes;
  }
  unretire(s);
  session_ = s;
  has_session_ = true;
  book_.clear();
  for (Slot& e : pending_) e.seq = 0;
  mode_ = Mode::kLive;
  expected_ = 1;
  known_next_ = 1;
  eos_ = false;
  eos_seq_ = 0;
  req_active_ = false;
  gap_open_ = false;
  snap_due_ = false;
  snap_anchor_ = 0;
  if (handler_) handler_->on_reset();
}

void SubscriberCore::enter_snapshot() {
  mode_ = Mode::kSnapshot;
  req_active_ = false;
  gap_open_ = false;
  snap_due_ = true;
  snap_anchor_ = 0;
}

void SubscriberCore::on_datagram(std::span<const std::uint8_t> dgram, Source src,
                                 std::uint64_t now_ns) {
  const auto pv = protocol::parse_packet(dgram);
  if (!pv) {
    ++stats_.malformed;
    return;
  }
  ++(src == Source::kFeed ? stats_.feed_packets : stats_.retransmit_packets);
  const protocol::PacketHeader& h = pv->header;
  if (h.seq == 0 || (has_session_ && h.session == session_ && h.seq > expected_ &&
                     h.seq - expected_ > kMaxSeqJump)) {
    ++stats_.malformed;
    return;
  }

  if (!has_session_ || h.session != session_) {
    // A delayed packet from the session we already left (UDP reordering), or
    // a reply about some other session: ignore it.
    if (is_retired(h.session) || src == Source::kRetransmit) {
      ++stats_.stale_packets;
      return;
    }
    // The publisher (re)started. State "as of 0" is empty, so a subscriber
    // that sees the session from its first sequence number needs nothing
    // else; one that joins later takes a snapshot.
    new_session(h.session);
    if (h.seq != 1) enter_snapshot();
  }

  switch (pv->kind) {
    case PacketKind::kEndOfSession:
      if (src == Source::kFeed) {
        eos_ = true;
        eos_seq_ = h.seq;
        known_next_ = std::max(known_next_, h.seq);
      }
      break;
    case PacketKind::kHeartbeat:
      if (src == Source::kRetransmit) {
        // NAK: h.seq is the oldest message the server still holds. If that is
        // past what we need, the gap can only be filled by a snapshot. (An
        // older NAK, overtaken by recovery, is ignored.)
        if (mode_ == Mode::kLive && h.seq > expected_) {
          ++stats_.naks;
          enter_snapshot();
        }
        return;
      }
      ++stats_.heartbeats;
      known_next_ = std::max(known_next_, h.seq);
      break;
    case PacketKind::kData:
      known_next_ = std::max(known_next_, h.seq + h.count);
      pv->for_each([&](std::uint64_t seq, std::span<const std::uint8_t> msg) { accept(seq, msg); });
      break;
  }
  maybe_request(now_ns);
}

void SubscriberCore::accept(std::uint64_t seq, std::span<const std::uint8_t> msg) {
  if (seq < expected_) {
    ++stats_.duplicates;
    return;
  }
  if (seq == expected_) {
    apply(seq, msg);
    ++expected_;
    drain_pending();
    return;
  }
  // Ahead of `expected`: buffer it if it is inside the window. While waiting
  // for a snapshot the window is anchored at the first message buffered,
  // because `expected` may be far behind (the snapshot will jump it).
  bool in_window = seq - expected_ < cfg_.window;
  if (!in_window && mode_ == Mode::kSnapshot) {
    if (snap_anchor_ == 0) snap_anchor_ = seq;
    in_window = seq >= snap_anchor_ && seq - snap_anchor_ < cfg_.window;
  }
  if (!in_window) {
    ++stats_.window_overflow;  // re-requested once `expected` gets closer
    return;
  }
  if (msg.size() > kMaxMessageBytes) {
    ++stats_.oversize;
    return;
  }
  Slot& e = pending_[seq & mask_];
  if (e.seq == seq) {
    ++stats_.duplicates;
    return;
  }
  // The slot may hold another message congruent mod the window (stale, or
  // anchored far ahead). Every lookup checks the exact sequence number, so
  // overwriting only costs a later re-request, never a wrong apply.
  e.seq = seq;
  e.len = static_cast<std::uint16_t>(msg.size());
  std::memcpy(e.bytes, msg.data(), msg.size());
  ++stats_.buffered;
}

void SubscriberCore::apply(std::uint64_t seq, std::span<const std::uint8_t> msg) {
  const ApplyResult r = book_.apply(msg);
  if (r == ApplyResult::kUnknownType || r == ApplyResult::kMalformed) ++stats_.unknown_messages;
  ++stats_.applied;
  if (handler_) handler_->on_message(seq, msg);
}

void SubscriberCore::drain_pending() {
  for (;;) {
    Slot& e = pending_[expected_ & mask_];
    if (e.seq != expected_) return;
    e.seq = 0;
    apply(expected_, std::span<const std::uint8_t>(e.bytes, e.len));
    ++expected_;
  }
}

void SubscriberCore::maybe_request(std::uint64_t now_ns) {
  if (mode_ != Mode::kLive) return;
  if (known_next_ <= expected_) {  // no gap
    req_active_ = false;
    gap_open_ = false;
    return;
  }
  if (!gap_open_) {
    gap_open_ = true;
    ++stats_.gaps;
  }
  if (io_ == nullptr) return;
  bool retry = false;
  if (req_active_) {
    const bool complete = expected_ >= req_to_;
    if (!complete && now_ns - req_sent_ns_ < cfg_.retransmit_timeout_ns) return;
    retry = !complete;
  }
  // Request up to the first message already buffered (or what is known).
  std::uint64_t hi = std::min(known_next_, expected_ + cfg_.max_request);
  for (std::uint64_t s = expected_ + 1; s < hi; ++s) {
    if (pending_[s & mask_].seq == s) {
      hi = s;
      break;
    }
  }
  protocol::RetransmitRequest r;
  r.session = session_;
  r.seq = expected_;
  r.count = static_cast<std::uint16_t>(hi - expected_);
  io_->send_request(r);
  req_active_ = true;
  req_to_ = hi;
  req_sent_ns_ = now_ns;
  ++stats_.requests;
  if (retry) ++stats_.request_retries;
}

void SubscriberCore::poll(std::uint64_t now_ns) { maybe_request(now_ns); }

bool SubscriberCore::take_snapshot_request(std::uint64_t now_ns) {
  if (mode_ != Mode::kSnapshot) return false;
  if (!snap_due_ && now_ns - snap_last_ns_ < cfg_.snapshot_retry_ns) return false;
  snap_due_ = false;
  snap_last_ns_ = now_ns;
  snap_req_session_ = session_;
  ++stats_.snapshot_requests;
  return true;
}

void SubscriberCore::on_snapshot_failed(std::uint64_t now_ns) {
  ++stats_.snapshot_failures;
  snap_last_ns_ = now_ns;  // retry after snapshot_retry_ns
}

void SubscriberCore::on_snapshot(const SnapshotData& snap, std::uint64_t now_ns) {
  if (mode_ != Mode::kSnapshot) return;  // not asked for (already recovered)
  if (!has_session_ || snap.session != session_) {
    // A snapshot from a session we left is normally stale (requested before
    // a restart, answered after). But if we asked for it from the current
    // session, the server -- the publisher itself -- is telling us its live
    // session: the current one came from a stray packet. Adopt it then, or
    // we would never recover.
    if (is_retired(snap.session) && snap_req_session_ != session_) {
      on_snapshot_failed(now_ns);
      return;
    }
    new_session(snap.session);
    mode_ = Mode::kSnapshot;
  }
  // Adopt the snapshot unless we already hold a later state (TLA+ Snapshot:
  // e == Max(expected, S + 1)). Our book is a prefix of the publisher's log,
  // so if expected <= S + 1 the snapshot supersedes it entirely.
  if (snap.as_of_seq + 1 >= expected_) {
    book_.clear();
    for (const protocol::TrackUpdate& u : snap.tracks) book_.put(u);
    expected_ = snap.as_of_seq + 1;
    if (handler_) handler_->on_snapshot_loaded();
  }
  known_next_ = std::max(known_next_, snap.as_of_seq + 1);
  mode_ = Mode::kLive;
  snap_anchor_ = 0;
  ++stats_.snapshots_loaded;
  drain_pending();
  maybe_request(now_ns);
}

// ---------------------------------------------------------------------------
// SubscriberClient
// ---------------------------------------------------------------------------

SubscriberClient::SubscriberClient(const SubscriberClientConfig& cfg)
    : cfg_(cfg), core_(cfg.core, this, nullptr), loss_(cfg.feed_loss) {}

SubscriberClient::~SubscriberClient() {
  close_fd(feed_fd_);
  close_fd(req_fd_);
}

bool SubscriberClient::open(std::string* error) {
  if (!resolve_ipv4(cfg_.retransmit, rtx_addr_, error)) return false;
  feed_fd_ = open_feed_receiver(cfg_.feed, cfg_.interface_host, cfg_.rcvbuf, error);
  if (feed_fd_ < 0) return false;
  req_fd_ = open_udp("0.0.0.0", 0, cfg_.rcvbuf, error);
  return req_fd_ >= 0;
}

void SubscriberClient::send_request(const protocol::RetransmitRequest& req) {
  std::uint8_t buf[protocol::RetransmitRequest::kWireSize];
  protocol::write(req, buf);
  (void)::sendto(req_fd_, buf, sizeof buf, 0, reinterpret_cast<const sockaddr*>(&rtx_addr_),
                 sizeof rtx_addr_);
}

void SubscriberClient::handle_snapshot(std::uint64_t now) {
  if (!core_.take_snapshot_request(now)) return;
  SnapshotData snap;
  if (fetch_snapshot(cfg_.snapshot, cfg_.snapshot_timeout_ms, snap, &snap_error_)) {
    core_.on_snapshot(snap, mono_ns());
  } else {
    core_.on_snapshot_failed(mono_ns());
  }
}

void SubscriberClient::run(const std::atomic<bool>& stop, bool stop_at_end) {
  pollfd fds[2] = {{feed_fd_, POLLIN, 0}, {req_fd_, POLLIN, 0}};
  std::uint8_t buf[2048];
  constexpr int kMaxPerPass = 256;  // interleave the two sockets under load
  while (!stop.load(std::memory_order_relaxed)) {
    if (stop_at_end && core_.ended()) break;
    const int rc = ::poll(fds, 2, 1);
    if (rc > 0) {
      for (int i = 0; i < kMaxPerPass; ++i) {
        const ssize_t n = ::recv(feed_fd_, buf, sizeof buf, 0);
        if (n < 0) break;
        if (loss_.drop()) continue;
        core_.on_datagram(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)),
                          SubscriberCore::Source::kFeed, mono_ns());
      }
      for (int i = 0; i < kMaxPerPass; ++i) {
        sockaddr_in from{};
        socklen_t flen = sizeof from;
        const ssize_t n = ::recvfrom(req_fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) break;
        // Only the retransmit server answers here. Match the port only: a
        // server bound to 0.0.0.0 on a multi-homed host may reply from a
        // different local address than the one we sent to.
        if (from.sin_port != rtx_addr_.sin_port) continue;
        core_.on_datagram(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)),
                          SubscriberCore::Source::kRetransmit, mono_ns());
      }
    }
    const std::uint64_t now = mono_ns();
    core_.poll(now);
    handle_snapshot(now);
    if (SubscriberHandler* h = core_.handler()) h->on_tick(now);
  }
}

bool SubscriberClient::fetch_snapshot(const Endpoint& server, std::uint32_t timeout_ms,
                                      SnapshotData& out, std::string* error) {
  const int fd = ingest::detail::tcp_connect(server.host, server.port, timeout_ms, 4 << 20,
                                             /*nonblocking=*/false, error);
  if (fd < 0) return false;
  timeval tv{};
  tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout_ms / 1000);
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout_ms % 1000) * 1000);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  const std::uint64_t deadline = mono_ns() + std::uint64_t{timeout_ms} * 1'000'000;
  SnapshotParser parser;
  std::vector<std::uint8_t> buf(64 * 1024);
  bool ok = false;
  for (;;) {
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n > 0) {
      const auto st = parser.feed(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)));
      if (st == SnapshotParser::State::kError) {
        if (error) *error = std::string("snapshot: ") + parser.error();
        break;
      }
      if (st == SnapshotParser::State::kDone) {  // complete: no need to wait for the close
        ok = true;
        break;
      }
      if (mono_ns() > deadline) {
        if (error) *error = "snapshot: timed out";
        break;
      }
      continue;
    }
    if (n == 0) {  // server closed: complete only if the stream was
      ok = parser.state() == SnapshotParser::State::kDone;
      if (!ok && error) *error = "snapshot: connection closed before SnapshotEnd";
      break;
    }
    if (errno == EINTR) continue;
    if (error) *error = std::string("snapshot: ") + std::strerror(errno);
    break;
  }
  ::close(fd);
  if (ok) out = std::move(parser.data());
  return ok;
}

}  // namespace adsb::feed
