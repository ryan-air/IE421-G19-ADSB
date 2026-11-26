#pragma once

// Converts a recorded Beast byte stream (e.g. a capture of dump1090-fa's TCP
// port 30005 written to disk) into .adsbrec records, with event time taken
// from the frames' own 12 MHz counter. The conversion is a pure function of
// the input bytes, so replays of an imported capture are deterministic.
//
//   rx_ns    = unwrapped ticks * 1e9 / 12e6 (floor), in the counter's own
//              domain: two windows imported from one capture line up.
//   radio_ts = the frame's raw 48-bit counter, unchanged.
//
// The counter is unwrapped across:
//  * a 48-bit wrap (previous value in the last minute before 2^48, next in
//    the first minute): continues exactly;
//  * a small backwards step (at most reorder_ns: frames written slightly out
//    of order, e.g. by a decoder with several demodulator threads): the frame
//    keeps the previous rx_ns, so time never runs backwards. Counted in
//    out_of_order. dump1090-fa's output is in order;
//  * any other backwards step (the decoder restarted and its counter began
//    again): rx_ns continues `reset_gap_ns` after the previous frame, since
//    the real length of the outage is unknown. Counted in clock_resets.
// Forward jumps longer than long_gap_ns are kept as they are (a quiet sky or
// an outage; the counter cannot tell) and counted.
//
// Receiver status frames (type '4') are not frames from aircraft: counted and
// skipped. Mode A/C and Mode S frames are kept.
//
// A time window [from_ns, until_ns), measured from the first frame of the
// input, selects part of a long capture; done() turns true at the first frame
// at or past until_ns so the caller can stop reading.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "adsb/ingest/beast.hpp"
#include "adsb/record/recording.hpp"

namespace adsb::ingest {

struct BeastImportConfig {
  std::uint64_t from_ns = 0;
  std::uint64_t until_ns = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t reorder_ns = 1'000'000'000;    // backwards steps up to 1 s are reordering
  std::uint64_t reset_gap_ns = 1'000'000;      // 1 ms after the last frame
  std::uint64_t long_gap_ns = 60'000'000'000;  // report forward jumps > 60 s
  std::uint8_t line = 0;
};

struct BeastImportStats {
  std::uint64_t frames = 0;          // frames parsed, all types
  std::uint64_t written = 0;         // records passed to the sink
  std::uint64_t mode_ac = 0;         // written, by type
  std::uint64_t mode_s_short = 0;
  std::uint64_t mode_s_long = 0;
  std::uint64_t status_skipped = 0;  // type '4' frames
  std::uint64_t outside_window = 0;  // aircraft frames before from_ns (or at/after until_ns)
  std::uint64_t out_of_order = 0;    // small backwards steps, given the previous rx_ns
  std::uint64_t clock_resets = 0;    // larger backwards steps other than a wrap
  std::uint64_t clock_wraps = 0;
  std::uint64_t long_gaps = 0;
  std::uint64_t longest_gap_ns = 0;
  std::uint64_t first_rx_ns = 0;     // first frame of the input (window origin)
  std::uint64_t last_rx_ns = 0;      // last frame seen
  std::uint64_t first_written_rx_ns = 0;
  std::uint64_t last_written_rx_ns = 0;
};

class BeastImporter {
 public:
  static constexpr std::uint64_t kCounterModulus = std::uint64_t{1} << 48;
  static constexpr std::uint64_t kTicksPerSecond = record::kBeastClockHz;

  explicit BeastImporter(const BeastImportConfig& cfg = {}) noexcept : cfg_(cfg) {}

  // Ticks of the 12 MHz counter -> ns (floor). Exact for any tick count below
  // 2^56 (no overflow: 2^56 * 250 < 2^64).
  [[nodiscard]] static constexpr std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept {
    return ticks / 3 * 250 + ticks % 3 * 250 / 3;
  }

  // Feeds `n` bytes; `sink(const record::Record&)` gets every record inside
  // the window, in input order (rx_ns never decreases). Returns records
  // written by this call. Bytes after done() are ignored.
  template <typename Sink>
  std::size_t feed(const std::uint8_t* data, std::size_t n, Sink&& sink) {
    std::size_t written = 0;
    if (done_) return 0;
    parser_.feed(data, n, [&](const BeastFrame& f) {
      if (done_) return;
      ++stats_.frames;
      if (f.type == BeastType::kStatus) {
        ++stats_.status_skipped;
        return;
      }
      const std::uint64_t rx = unwrap(f.timestamp);
      const std::uint64_t elapsed = rx - stats_.first_rx_ns;
      if (elapsed >= cfg_.until_ns) {
        ++stats_.outside_window;
        done_ = true;
        return;
      }
      if (elapsed < cfg_.from_ns) {
        ++stats_.outside_window;
        return;
      }
      record::Record r;
      r.rx_ns = rx;
      r.radio_ts = f.timestamp;
      r.line = cfg_.line;
      r.type = static_cast<std::uint8_t>(f.type);
      r.signal = f.signal;
      r.len = f.len;
      std::memcpy(r.payload, f.payload, f.len);
      switch (f.type) {
        case BeastType::kModeAC: ++stats_.mode_ac; break;
        case BeastType::kModeSShort: ++stats_.mode_s_short; break;
        default: ++stats_.mode_s_long; break;
      }
      if (stats_.written == 0) stats_.first_written_rx_ns = rx;
      stats_.last_written_rx_ns = rx;
      ++stats_.written;
      ++written;
      sink(static_cast<const record::Record&>(r));
    });
    return written;
  }

  [[nodiscard]] bool done() const noexcept { return done_; }
  [[nodiscard]] const BeastImportStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const BeastStats& parser_stats() const noexcept { return parser_.stats(); }

 private:
  std::uint64_t unwrap(std::uint64_t ts) noexcept {
    constexpr std::uint64_t kMinute = 60 * kTicksPerSecond;
    if (!started_) {
      started_ = true;
      prev_ts_ = ts;
      prev_ext_ = ts;
      stats_.first_rx_ns = stats_.last_rx_ns = ticks_to_ns(ts);
      return stats_.first_rx_ns;
    }
    std::uint64_t ext;
    if (ts >= prev_ts_) {
      ext = prev_ext_ + (ts - prev_ts_);
    } else if (prev_ts_ >= kCounterModulus - kMinute && ts < kMinute) {
      ++stats_.clock_wraps;
      ext = prev_ext_ + (kCounterModulus - prev_ts_) + ts;
    } else if (ticks_to_ns(prev_ts_ - ts) <= cfg_.reorder_ns) {
      // Keep the latest counter value as the reference: the next in-order
      // frame is measured from it, not from this late one.
      ++stats_.out_of_order;
      return stats_.last_rx_ns;
    } else {
      ++stats_.clock_resets;
      // Round the gap up to whole ticks so rx_ns moves by at least reset_gap_ns.
      ext = prev_ext_ + (cfg_.reset_gap_ns * 3 + 249) / 250;
    }
    const std::uint64_t rx = ticks_to_ns(ext);
    const std::uint64_t gap = rx - stats_.last_rx_ns;
    if (gap > stats_.longest_gap_ns) stats_.longest_gap_ns = gap;
    if (gap > cfg_.long_gap_ns) ++stats_.long_gaps;
    prev_ts_ = ts;
    prev_ext_ = ext;
    stats_.last_rx_ns = rx;
    return rx;
  }

  BeastImportConfig cfg_;
  BeastParser parser_;
  BeastImportStats stats_{};
  bool started_ = false;
  bool done_ = false;
  std::uint64_t prev_ts_ = 0;   // raw counter of the previous frame
  std::uint64_t prev_ext_ = 0;  // unwrapped ticks of the previous frame
};

}  // namespace adsb::ingest
