#pragma once

// MoldUDP64-style packet framing for the feed (protocol/feed_spec.md §2).
//
//   PacketHeader (20 B)  { session[10], seq u64, count u16 }
//   count x { u16 length, length bytes }
//
// * seq is the sequence number of the FIRST message; message i has seq + i.
// * count == 0 is a heartbeat (seq = next sequence number);
//   count == 0xFFFF marks the end of the session.
//
// PacketBuilder assembles datagrams in a fixed buffer (no allocation).
// parse_packet() validates a received datagram without trusting any length.
// SequenceTracker is the subscriber-side gap detector.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "adsb/protocol/feed_messages.hpp"

namespace adsb::protocol {

using Session = std::array<char, kSessionLength>;

// Builds one session id from a 10-character string (shorter is NUL-padded).
[[nodiscard]] inline Session make_session(const char* text) noexcept {
  Session s{};
  for (std::size_t i = 0; i < s.size() && text[i] != '\0'; ++i) s[i] = text[i];
  return s;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

class PacketBuilder {
 public:
  static constexpr std::size_t kCapacity = kMaxUdpPayload;

  PacketBuilder(const Session& session, std::uint64_t first_seq) noexcept
      : session_(session), first_seq_(first_seq) {}

  // Appends one message. Returns false (and changes nothing) if it would not
  // fit in a single datagram or the packet already holds 65534 messages.
  [[nodiscard]] bool add(const std::uint8_t* msg, std::uint16_t len) noexcept {
    if (size_ + 2u + len > kCapacity || count_ == kEndOfSessionCount - 1) return false;
    std::memcpy(buf_.data() + size_, &len, 2);
    std::memcpy(buf_.data() + size_ + 2, msg, len);
    size_ += 2u + len;
    ++count_;
    return true;
  }

  // Convenience for generated message types.
  template <typename Msg>
  [[nodiscard]] bool add(const Msg& m) noexcept {
    std::uint8_t tmp[Msg::kWireSize];
    write(m, tmp);
    return add(tmp, static_cast<std::uint16_t>(Msg::kWireSize));
  }

  [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] std::uint64_t first_seq() const noexcept { return first_seq_; }
  [[nodiscard]] std::uint64_t next_seq() const noexcept { return first_seq_ + count_; }

  // Writes the header and returns the finished datagram. The builder is left
  // ready for the next packet, starting at next_seq().
  [[nodiscard]] std::span<const std::uint8_t> finish() noexcept {
    stamp_header(first_seq_, count_);
    const std::span<const std::uint8_t> out(buf_.data(), size_);
    first_seq_ += count_;
    count_ = 0;
    size_ = PacketHeader::kWireSize;
    return out;
  }

  // A heartbeat (count 0, seq = next sequence number) or end-of-session
  // marker. Only valid when no messages are pending.
  [[nodiscard]] std::span<const std::uint8_t> heartbeat(bool end_of_session = false) noexcept {
    stamp_header(first_seq_, end_of_session ? kEndOfSessionCount : kHeartbeatCount);
    return {buf_.data(), PacketHeader::kWireSize};
  }

 private:
  void stamp_header(std::uint64_t seq, std::uint16_t count) noexcept {
    PacketHeader h;
    h.session = session_;
    h.seq = seq;
    h.count = count;
    write(h, buf_.data());
  }

  Session session_;
  std::uint64_t first_seq_;
  std::uint16_t count_ = 0;
  std::size_t size_ = PacketHeader::kWireSize;
  std::array<std::uint8_t, kCapacity> buf_{};
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

enum class PacketKind : std::uint8_t { kData, kHeartbeat, kEndOfSession };

struct PacketView {
  PacketHeader header;
  PacketKind kind = PacketKind::kData;
  std::span<const std::uint8_t> body;  // the message blocks

  // Calls fn(seq, std::span<const uint8_t> message) for each message.
  // parse_packet() has already validated every length, so this cannot overrun.
  template <typename Fn>
  void for_each(Fn&& fn) const {
    if (kind != PacketKind::kData) return;
    std::size_t off = 0;
    for (std::uint16_t i = 0; i < header.count; ++i) {
      std::uint16_t len;
      std::memcpy(&len, body.data() + off, 2);
      fn(header.seq + i, body.subspan(off + 2, len));
      off += 2u + len;
    }
  }
};

enum class ParseError : std::uint8_t { kNone, kTooShort, kTruncated, kTrailingBytes };

// Validates framing: header present, every block length within the datagram,
// and no bytes left over. Message contents are not interpreted.
[[nodiscard]] inline std::optional<PacketView> parse_packet(std::span<const std::uint8_t> dgram,
                                                            ParseError* err = nullptr) noexcept {
  auto fail = [&](ParseError e) -> std::optional<PacketView> {
    if (err) *err = e;
    return std::nullopt;
  };
  if (err) *err = ParseError::kNone;
  if (dgram.size() < PacketHeader::kWireSize) return fail(ParseError::kTooShort);
  PacketView v;
  read(dgram.data(), v.header);
  v.body = dgram.subspan(PacketHeader::kWireSize);
  if (v.header.count == kEndOfSessionCount) {
    v.kind = PacketKind::kEndOfSession;
    return v.body.empty() ? std::optional<PacketView>(v) : fail(ParseError::kTrailingBytes);
  }
  if (v.header.count == kHeartbeatCount) {
    v.kind = PacketKind::kHeartbeat;
    return v.body.empty() ? std::optional<PacketView>(v) : fail(ParseError::kTrailingBytes);
  }
  std::size_t off = 0;
  for (std::uint16_t i = 0; i < v.header.count; ++i) {
    if (off + 2 > v.body.size()) return fail(ParseError::kTruncated);
    std::uint16_t len;
    std::memcpy(&len, v.body.data() + off, 2);
    off += 2;
    if (off + len > v.body.size()) return fail(ParseError::kTruncated);
    off += len;
  }
  if (off != v.body.size()) return fail(ParseError::kTrailingBytes);
  return v;
}

// ---------------------------------------------------------------------------
// Subscriber-side sequencing
// ---------------------------------------------------------------------------

// Classifies each packet against the next expected sequence number. This is
// the decision logic of the recovery protocol modelled in spec/; buffering,
// retransmit requests, and snapshot fallback are built on top of it.
class SequenceTracker {
 public:
  enum class Result : std::uint8_t {
    kInOrder,     // first message == expected; deliver all, expected advances
    kOverlap,     // starts before expected but extends past it: skip `skip` msgs, deliver rest
    kDuplicate,   // entirely before expected: drop
    kGap,         // starts after expected: messages [gap_from, gap_to) are missing
    kNewSession,  // unknown session: publisher restarted. Caller takes a snapshot, then reset()
    kStaleSession,  // late packet from the session we already left: drop it
    kHeartbeat,   // no messages; may still reveal a gap (see gap_from/gap_to)
    kEndOfSession,
    kMalformed,   // seq 0, or seq + count past 2^64 - 1: nothing changes
  };

  struct Outcome {
    Result result = Result::kInOrder;
    std::uint16_t skip = 0;        // leading messages already seen (kOverlap)
    std::uint64_t gap_from = 0;    // first missing seq (kGap / kHeartbeat with gap)
    std::uint64_t gap_to = 0;      // one past the last missing seq
  };

  explicit SequenceTracker(std::uint64_t expected = 1) noexcept : expected_(expected) {}

  // Feeds one parsed packet. For kGap the tracker does NOT advance: the caller
  // buffers the packet, recovers [gap_from, gap_to), then calls advance_to().
  [[nodiscard]] Outcome on_packet(const PacketView& p) noexcept {
    Outcome o;
    // Sequence numbers start at 1 and never wrap within a session, so a
    // packet claiming seq 0, or one whose messages would run past 2^64 - 1,
    // is corrupt or forged (end = seq + count would wrap `expected`
    // backwards). Checked before the session, as SubscriberCore does, so a
    // corrupt packet cannot pass for a publisher restart. The end-of-session
    // count is a marker, not a message count.
    if (p.header.seq == 0 ||
        (p.kind == PacketKind::kData && p.header.count > ~std::uint64_t{0} - p.header.seq)) {
      o.result = Result::kMalformed;
      return o;
    }
    if (has_session_ && p.header.session != session_) {
      // UDP can deliver a delayed packet from the previous session after we
      // switched; recognise it rather than switching back (which would force
      // a snapshot ping-pong). State is not changed until the caller resets.
      o.result = (has_retired_ && p.header.session == retired_) ? Result::kStaleSession
                                                                : Result::kNewSession;
      return o;
    }
    if (!has_session_) {
      session_ = p.header.session;
      has_session_ = true;
    }
    const std::uint64_t first = p.header.seq;
    if (p.kind == PacketKind::kEndOfSession) {
      o.result = Result::kEndOfSession;
      return o;
    }
    if (p.kind == PacketKind::kHeartbeat) {
      o.result = Result::kHeartbeat;
      if (first > expected_) {
        o.gap_from = expected_;
        o.gap_to = first;
      }
      return o;
    }
    const std::uint64_t end = first + p.header.count;  // one past the last
    if (first == expected_) {
      expected_ = end;
      o.result = Result::kInOrder;
    } else if (first > expected_) {
      o.result = Result::kGap;
      o.gap_from = expected_;
      o.gap_to = first;
    } else if (end <= expected_) {
      o.result = Result::kDuplicate;
    } else {
      o.result = Result::kOverlap;
      o.skip = static_cast<std::uint16_t>(expected_ - first);
      expected_ = end;
    }
    return o;
  }

  // After the caller has filled a gap (retransmission or snapshot).
  void advance_to(std::uint64_t next_expected) noexcept {
    if (next_expected > expected_) expected_ = next_expected;
  }

  // Resets for a new session starting at `expected` (after a snapshot).
  void reset(const Session& session, std::uint64_t expected) noexcept {
    if (has_session_ && session != session_) {
      retired_ = session_;
      has_retired_ = true;
    }
    session_ = session;
    has_session_ = true;
    expected_ = expected;
  }

  [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }

 private:
  Session session_{};
  bool has_session_ = false;
  Session retired_{};  // the session before the last reset()
  bool has_retired_ = false;
  std::uint64_t expected_;
};

}  // namespace adsb::protocol
