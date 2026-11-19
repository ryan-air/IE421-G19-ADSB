#include "adsb/feed/book.hpp"

#include <algorithm>
#include <cstring>

#include "adsb/common/hash.hpp"

namespace adsb::feed {

using protocol::TrackDelete;
using protocol::TrackUpdate;

ApplyResult FeedBook::apply(std::span<const std::uint8_t> msg) {
  if (msg.empty()) return ApplyResult::kMalformed;
  switch (msg[0]) {
    case static_cast<std::uint8_t>(TrackUpdate::kTypeChar): {
      // Longer is allowed: later versions may only append fields.
      if (msg.size() < TrackUpdate::kWireSize) return ApplyResult::kMalformed;
      TrackUpdate u;
      protocol::read(msg.data(), u);
      tracks_[u.icao] = u;
      return ApplyResult::kUpdate;
    }
    case static_cast<std::uint8_t>(TrackDelete::kTypeChar): {
      if (msg.size() < TrackDelete::kWireSize) return ApplyResult::kMalformed;
      TrackDelete d;
      protocol::read(msg.data(), d);
      return tracks_.erase(d.icao) ? ApplyResult::kDelete : ApplyResult::kDeleteUnknown;
    }
    default:
      return ApplyResult::kUnknownType;
  }
}

const TrackUpdate* FeedBook::find(std::uint32_t icao) const {
  const auto it = tracks_.find(icao);
  return it == tracks_.end() ? nullptr : &it->second;
}

std::vector<TrackUpdate> FeedBook::sorted() const {
  std::vector<TrackUpdate> out;
  out.reserve(tracks_.size());
  for (const auto& kv : tracks_) out.push_back(kv.second);
  std::sort(out.begin(), out.end(),
            [](const TrackUpdate& a, const TrackUpdate& b) { return a.icao < b.icao; });
  return out;
}

std::array<std::uint8_t, 32> FeedBook::digest() const {
  Sha256 sha;
  std::uint8_t buf[TrackUpdate::kWireSize];
  for (const TrackUpdate& u : sorted()) {
    protocol::write(u, buf);
    sha.update(buf, sizeof buf);
  }
  return sha.finish();
}

std::string FeedBook::hex_digest() const { return Sha256::hex(digest()); }

std::uint64_t FeedBook::track_fingerprint(const TrackUpdate& u) noexcept {
  std::uint8_t buf[TrackUpdate::kWireSize];
  protocol::write(u, buf);
  // splitmix64 finalizer over each 8-byte word, chained.
  std::uint64_t h = 0x9E3779B97F4A7C15ull;
  for (std::size_t i = 0; i < sizeof buf; i += 8) {
    std::uint64_t w;
    std::memcpy(&w, buf + i, 8);
    h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
  }
  return h;
}

std::uint64_t FeedBook::fingerprint() const noexcept {
  std::uint64_t sum = 0;
  for (const auto& kv : tracks_) sum += track_fingerprint(kv.second);
  return sum;
}

std::array<std::uint8_t, 32> FeedBook::state_digest() const {
  Sha256 sha;
  std::uint8_t buf[TrackUpdate::kWireSize];
  for (TrackUpdate u : sorted()) {
    u.update_mask = 0;
    u.publish_ts_ns = 0;
    protocol::write(u, buf);
    sha.update(buf, sizeof buf);
  }
  return sha.finish();
}

std::string FeedBook::hex_state_digest() const { return Sha256::hex(state_digest()); }

bool FeedBook::operator==(const FeedBook& other) const {
  if (tracks_.size() != other.tracks_.size()) return false;
  std::uint8_t a[TrackUpdate::kWireSize];
  std::uint8_t b[TrackUpdate::kWireSize];
  for (const auto& kv : tracks_) {
    const TrackUpdate* o = other.find(kv.first);
    if (o == nullptr) return false;
    protocol::write(kv.second, a);
    protocol::write(*o, b);
    if (std::memcmp(a, b, sizeof a) != 0) return false;
  }
  return true;
}

}  // namespace adsb::feed
