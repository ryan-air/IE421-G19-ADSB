#pragma once

// Streaming parser for the Mode-S Beast binary protocol (dump1090 / readsb
// TCP port 30005).
//
// Frame layout, before escaping:
//
//     0x1A  <type>  <timestamp: 6 bytes, big-endian>  <signal: 1 byte>  <payload>
//
//     type '1' (0x31) Mode A/C        payload  2 bytes
//          '2' (0x32) Mode S short    payload  7 bytes
//          '3' (0x33) Mode S long     payload 14 bytes
//          '4' (0x34) receiver status payload 14 bytes (Radarcape-style)
//
// Every 0x1A byte in the timestamp, signal, or payload is sent twice
// ("1A 1A"). The timestamp is a free-running 12 MHz counter.
//
// The parser is a resumable state machine: bytes may arrive in arbitrarily
// sized chunks (frames split across recv() calls). It never allocates, never
// throws, and resynchronizes after corruption:
//  * an unknown type byte after 0x1A drops back to hunting for 0x1A;
//  * a lone (unescaped) 0x1A inside a frame means the previous frame was
//    truncated: it is discarded and the 0x1A starts the next frame.
// One ambiguity is inherent to the protocol (readsb/dump1090 share it): while
// hunting, "1A 1A" is read as an escaped data byte, so a stray lone 0x1A
// immediately before a real frame start costs that one frame.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace adsb::ingest {

enum class BeastType : std::uint8_t {
  kModeAC = '1',
  kModeSShort = '2',
  kModeSLong = '3',
  kStatus = '4',
};

struct BeastFrame {
  BeastType type = BeastType::kModeSLong;
  std::uint8_t len = 0;           // payload length in bytes
  std::uint8_t signal = 0;        // raw signal level byte
  std::uint64_t timestamp = 0;    // 48-bit 12 MHz counter
  std::uint8_t payload[14] = {};
};

struct BeastStats {
  std::uint64_t frames = 0;           // frames emitted
  std::uint64_t bytes = 0;            // bytes consumed
  std::uint64_t bad_type = 0;         // 0x1A followed by an unknown type byte
  std::uint64_t truncated = 0;        // frame cut short by an unescaped 0x1A
  std::uint64_t skipped_bytes = 0;    // bytes discarded while hunting for sync
};

// Payload length for a type byte, or 0 if the type is unknown.
[[nodiscard]] constexpr std::uint8_t beast_payload_len(std::uint8_t type) noexcept {
  switch (type) {
    case '1': return 2;
    case '2': return 7;
    case '3': return 14;
    case '4': return 14;
    default: return 0;
  }
}

class BeastParser {
 public:
  static constexpr std::uint8_t kEscape = 0x1A;
  static constexpr std::size_t kHeaderLen = 7;  // 6 timestamp + 1 signal

  // Feeds `n` bytes. `on_frame(const BeastFrame&)` is invoked synchronously
  // for every complete frame. Returns the number of frames emitted.
  template <typename Sink>
  std::size_t feed(const std::uint8_t* data, std::size_t n, Sink&& on_frame) {
    std::size_t emitted = 0;
    stats_.bytes += n;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint8_t c = data[i];
      switch (state_) {
        case State::kHunt:
          if (c == kEscape) {
            state_ = State::kType;
          } else {
            ++stats_.skipped_bytes;
          }
          break;

        case State::kType:
          begin_frame(c);
          break;

        case State::kBody:
          if (c == kEscape) {
            state_ = State::kBodyEscape;
          } else {
            emitted += append(c, on_frame);
          }
          break;

        case State::kBodyEscape:
          if (c == kEscape) {
            // "1A 1A" -> one literal 0x1A data byte.
            state_ = State::kBody;
            emitted += append(kEscape, on_frame);
          } else {
            // A lone 0x1A: the current frame was truncated and a new frame
            // starts here, with `c` as its type byte.
            ++stats_.truncated;
            begin_frame(c);
          }
          break;
      }
    }
    stats_.frames += emitted;
    return emitted;
  }

  // Discards any partial frame (e.g. after a reconnect).
  void reset() noexcept {
    state_ = State::kHunt;
    filled_ = 0;
  }

  [[nodiscard]] const BeastStats& stats() const noexcept { return stats_; }

 private:
  enum class State : std::uint8_t { kHunt, kType, kBody, kBodyEscape };

  void begin_frame(std::uint8_t type) noexcept {
    const std::uint8_t len = beast_payload_len(type);
    if (len == 0) {
      // "1A 1A" while hunting is an escaped data byte, not a frame start;
      // anything else is an unknown type. Either way, keep hunting.
      if (type != kEscape) ++stats_.bad_type;
      stats_.skipped_bytes += 2;  // the 0x1A and the byte after it
      state_ = State::kHunt;
      return;
    }
    cur_.type = static_cast<BeastType>(type);
    cur_.len = len;
    needed_ = kHeaderLen + len;
    filled_ = 0;
    state_ = State::kBody;
  }

  template <typename Sink>
  std::size_t append(std::uint8_t c, Sink& on_frame) {
    body_[filled_++] = c;
    if (filled_ < needed_) return 0;

    std::uint64_t ts = 0;
    for (std::size_t k = 0; k < 6; ++k) ts = (ts << 8) | body_[k];
    cur_.timestamp = ts;
    cur_.signal = body_[6];
    std::memcpy(cur_.payload, body_ + kHeaderLen, cur_.len);
    if (cur_.len < sizeof(cur_.payload)) {
      std::memset(cur_.payload + cur_.len, 0, sizeof(cur_.payload) - cur_.len);
    }
    state_ = State::kHunt;
    filled_ = 0;
    on_frame(static_cast<const BeastFrame&>(cur_));
    return 1;
  }

  State state_ = State::kHunt;
  std::size_t needed_ = 0;
  std::size_t filled_ = 0;
  std::uint8_t body_[kHeaderLen + 14] = {};
  BeastFrame cur_{};
  BeastStats stats_{};
};

// Worst-case encoded size: 0x1A + type, then every body byte escaped.
inline constexpr std::size_t kBeastMaxEncodedLen = 2 + 2 * (BeastParser::kHeaderLen + 14);

// Serializes one frame into Beast wire format (with escaping). `out` must have
// room for kBeastMaxEncodedLen bytes. Returns the number of bytes written.
// Used by tests, the fuzzer, and (later) the replayer / load generator.
inline std::size_t beast_encode(const BeastFrame& f, std::uint8_t* out) noexcept {
  std::size_t n = 0;
  auto put = [&](std::uint8_t c) {
    out[n++] = c;
    if (c == BeastParser::kEscape) out[n++] = c;
  };
  out[n++] = BeastParser::kEscape;
  out[n++] = static_cast<std::uint8_t>(f.type);
  for (int k = 5; k >= 0; --k) put(static_cast<std::uint8_t>(f.timestamp >> (8 * k)));
  put(f.signal);
  for (std::size_t k = 0; k < f.len; ++k) put(f.payload[k]);
  return n;
}

}  // namespace adsb::ingest
