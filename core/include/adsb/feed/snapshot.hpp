#pragma once

// Snapshot stream: what the snapshot server writes on a TCP connection
// (protocol/feed_spec.md §3).
//
//   session        char[10]   the publisher session the snapshot belongs to
//   then u16-length-prefixed messages:
//     SnapshotBegin { track_count, as_of_seq, event_ts_ns }
//     TrackUpdate x track_count      (ICAO order)
//     SnapshotEnd   { as_of_seq }
//
// The server closes the connection after SnapshotEnd. The snapshot reflects
// every incremental message with seq <= as_of_seq.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "adsb/feed/book.hpp"
#include "adsb/protocol/moldudp64.hpp"

namespace adsb::feed {

struct SnapshotData {
  protocol::Session session{};
  std::uint64_t as_of_seq = 0;
  std::uint64_t event_ts_ns = 0;
  std::vector<protocol::TrackUpdate> tracks;
};

// Appends the whole stream for `book` to `out`.
void serialize_snapshot(const protocol::Session& session, std::uint64_t as_of_seq,
                        std::uint64_t event_ts_ns, const FeedBook& book,
                        std::vector<std::uint8_t>& out);

// Incremental parser for the stream (bytes can arrive in any chunking).
// Validates everything: message lengths, types, the declared track count,
// matching as_of_seq, and no bytes after SnapshotEnd.
class SnapshotParser {
 public:
  enum class State : std::uint8_t { kNeedMore, kDone, kError };

  // Upper bound on track_count, so a corrupt header cannot make the parser
  // reserve gigabytes. Far above any real airspace (~10k aircraft).
  static constexpr std::uint32_t kMaxTracks = 1u << 20;

  State feed(std::span<const std::uint8_t> bytes);
  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] const char* error() const noexcept { return error_; }

  // Valid once state() == kDone.
  [[nodiscard]] const SnapshotData& data() const noexcept { return data_; }
  [[nodiscard]] SnapshotData& data() noexcept { return data_; }

 private:
  State fail(const char* why);
  State on_message(std::span<const std::uint8_t> msg);

  enum class Stage : std::uint8_t { kSession, kBegin, kTracks, kEnd, kAfterEnd };
  Stage stage_ = Stage::kSession;
  State state_ = State::kNeedMore;
  const char* error_ = "";
  std::vector<std::uint8_t> pending_;  // bytes of an incomplete item
  std::uint32_t track_count_ = 0;
  SnapshotData data_;
};

}  // namespace adsb::feed
