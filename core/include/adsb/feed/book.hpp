#pragma once

// FeedBook: the table a track feed describes (ICAO address -> latest
// TrackUpdate), built by applying feed messages in sequence order.
//
// The publisher's recovery side keeps one (it is what the snapshot server
// serves), and every subscriber keeps one. The recovery protocol is correct
// when a subscriber's book is byte-identical to the publisher's at the same
// sequence number; digest() makes that comparison cheap (README_UPDATES.md
// §9.5 item 4).
//
// Not on the publisher's hot path: it lives on the recovery thread and in
// subscribers, so it uses std::unordered_map.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "adsb/protocol/feed_messages.hpp"

namespace adsb::feed {

enum class ApplyResult : std::uint8_t {
  kUpdate,         // TrackUpdate stored (new or replaced)
  kDelete,         // TrackDelete removed a track
  kDeleteUnknown,  // TrackDelete for a track the book does not hold
  kUnknownType,    // message type this version does not know: skipped (feed_spec.md §2)
  kMalformed,      // shorter than its type's wire size, or empty
};

class FeedBook {
 public:
  // Applies one feed message (the bytes of one message block).
  ApplyResult apply(std::span<const std::uint8_t> msg);

  void put(const protocol::TrackUpdate& u) { tracks_[u.icao] = u; }
  bool erase(std::uint32_t icao) { return tracks_.erase(icao) != 0; }
  void clear() noexcept { tracks_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return tracks_.size(); }
  [[nodiscard]] bool empty() const noexcept { return tracks_.empty(); }
  [[nodiscard]] const protocol::TrackUpdate* find(std::uint32_t icao) const;

  // Every track, ordered by ICAO address (deterministic).
  [[nodiscard]] std::vector<protocol::TrackUpdate> sorted() const;

  // SHA-256 over the wire bytes of every track in ICAO order. Two books have
  // the same digest exactly when they hold the same tracks byte for byte.
  [[nodiscard]] std::array<std::uint8_t, 32> digest() const;
  [[nodiscard]] std::string hex_digest() const;

  [[nodiscard]] bool operator==(const FeedBook& other) const;

 private:
  std::unordered_map<std::uint32_t, protocol::TrackUpdate> tracks_;
};

}  // namespace adsb::feed
