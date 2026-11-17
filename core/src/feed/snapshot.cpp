#include "adsb/feed/snapshot.hpp"

#include <algorithm>
#include <cstring>

namespace adsb::feed {

using protocol::SnapshotBegin;
using protocol::SnapshotEnd;
using protocol::TrackUpdate;

namespace {

template <typename Msg>
void append_msg(const Msg& m, std::vector<std::uint8_t>& out) {
  const auto len = static_cast<std::uint16_t>(Msg::kWireSize);
  const std::size_t at = out.size();
  out.resize(at + 2 + Msg::kWireSize);
  std::memcpy(out.data() + at, &len, 2);
  protocol::write(m, out.data() + at + 2);
}

}  // namespace

void serialize_snapshot(const protocol::Session& session, std::uint64_t as_of_seq,
                        std::uint64_t event_ts_ns, const FeedBook& book,
                        std::vector<std::uint8_t>& out) {
  const std::vector<TrackUpdate> tracks = book.sorted();
  out.reserve(out.size() + session.size() + 2 + SnapshotBegin::kWireSize +
              tracks.size() * (2 + TrackUpdate::kWireSize) + 2 + SnapshotEnd::kWireSize);
  out.insert(out.end(), session.begin(), session.end());
  SnapshotBegin b;
  b.track_count = static_cast<std::uint32_t>(tracks.size());
  b.as_of_seq = as_of_seq;
  b.event_ts_ns = event_ts_ns;
  append_msg(b, out);
  for (const TrackUpdate& u : tracks) append_msg(u, out);
  SnapshotEnd e;
  e.as_of_seq = as_of_seq;
  append_msg(e, out);
}

SnapshotParser::State SnapshotParser::fail(const char* why) {
  error_ = why;
  state_ = State::kError;
  return state_;
}

SnapshotParser::State SnapshotParser::feed(std::span<const std::uint8_t> bytes) {
  std::size_t i = 0;
  while (i < bytes.size() && state_ != State::kError) {
    if (stage_ == Stage::kAfterEnd) return fail("bytes after SnapshotEnd");
    if (stage_ == Stage::kSession) {
      const std::size_t want = data_.session.size() - pending_.size();
      const std::size_t take = std::min(want, bytes.size() - i);
      pending_.insert(pending_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i),
                      bytes.begin() + static_cast<std::ptrdiff_t>(i + take));
      i += take;
      if (pending_.size() == data_.session.size()) {
        std::memcpy(data_.session.data(), pending_.data(), data_.session.size());
        pending_.clear();
        stage_ = Stage::kBegin;
      }
      continue;
    }
    // A length-prefixed message: gather the 2-byte length, then the body.
    if (pending_.size() < 2) {
      pending_.push_back(bytes[i++]);
      continue;
    }
    std::uint16_t len;
    std::memcpy(&len, pending_.data(), 2);
    const std::size_t want = 2u + len - pending_.size();
    const std::size_t take = std::min(want, bytes.size() - i);
    pending_.insert(pending_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i),
                    bytes.begin() + static_cast<std::ptrdiff_t>(i + take));
    i += take;
    if (pending_.size() == 2u + len) {
      const std::vector<std::uint8_t> msg(pending_.begin() + 2, pending_.end());
      pending_.clear();
      on_message(msg);
    }
  }
  return state_;
}

SnapshotParser::State SnapshotParser::on_message(std::span<const std::uint8_t> msg) {
  if (msg.empty()) return fail("empty message");
  const std::uint8_t type = msg[0];
  switch (stage_) {
    case Stage::kBegin: {
      if (type != static_cast<std::uint8_t>(SnapshotBegin::kTypeChar) ||
          msg.size() != SnapshotBegin::kWireSize) {
        return fail("expected SnapshotBegin");
      }
      SnapshotBegin b;
      protocol::read(msg.data(), b);
      if (b.track_count > kMaxTracks) return fail("track_count too large");
      track_count_ = b.track_count;
      data_.as_of_seq = b.as_of_seq;
      data_.event_ts_ns = b.event_ts_ns;
      data_.tracks.clear();
      data_.tracks.reserve(track_count_);
      stage_ = track_count_ == 0 ? Stage::kEnd : Stage::kTracks;
      return state_;
    }
    case Stage::kTracks: {
      if (type != static_cast<std::uint8_t>(TrackUpdate::kTypeChar) ||
          msg.size() != TrackUpdate::kWireSize) {
        return fail("expected TrackUpdate");
      }
      TrackUpdate u;
      protocol::read(msg.data(), u);
      if (!data_.tracks.empty() && u.icao <= data_.tracks.back().icao) {
        return fail("tracks not in strictly increasing ICAO order");
      }
      data_.tracks.push_back(u);
      if (data_.tracks.size() == track_count_) stage_ = Stage::kEnd;
      return state_;
    }
    case Stage::kEnd: {
      if (type != static_cast<std::uint8_t>(SnapshotEnd::kTypeChar) ||
          msg.size() != SnapshotEnd::kWireSize) {
        return fail("expected SnapshotEnd");
      }
      SnapshotEnd e;
      protocol::read(msg.data(), e);
      if (e.as_of_seq != data_.as_of_seq) return fail("SnapshotEnd as_of_seq mismatch");
      stage_ = Stage::kAfterEnd;
      state_ = State::kDone;
      return state_;
    }
    default:
      return fail("unexpected message");
  }
}

}  // namespace adsb::feed
