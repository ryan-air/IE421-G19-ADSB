#pragma once

// A/B line arbitration (README_UPDATES.md §6).
//
// Two (up to four) receivers at one site hear the same transmissions. Like an
// exchange feed handler with A and B feeds, the arbiter forwards the first copy
// of every frame and drops the later copies, so the output has the union of
// what the lines received, and each frame arrives as early as the fastest line
// delivered it. ADS-B has no sequence numbers: a frame's identity is its
// payload (56 or 112 bits) within a short window (default 5 ms). Squitters
// with identical payloads (velocity, identification, DF11) are at least
// ~400 ms apart. Replies to interrogations can repeat byte for byte within
// milliseconds (several radars, a dwell of all-calls); such a repeat inside
// the window is dropped like a copy. It carries the same content, so the
// track state loses nothing, but the counters treat it as a copy. Mode A/C
// replies (2 bytes, shared by every aircraft on the same code) are passed
// through unarbitrated.
//
// offer() is the whole hot path: expire old entries, one open-addressing
// lookup, insert or count. No allocation after construction, no waiting for
// the slower line.
//
// Time is the record's rx_ns (event time), so a replay of a recording makes
// the same decisions as the live run that wrote it. Frames must be offered in
// nondecreasing rx_ns, as a recording stores them and as adsb-live's merge
// delivers them; an earlier time is raised to the latest one seen (counted in
// `time_backwards`).
//
// Measurements (§6.3): per line received, first copies (wins), late copies,
// frames only that line received; the win margin (later copy's rx_ns minus
// the first's) per winning line; and, for lines 0 and 1, the relative radio
// clock: radio_ts(1) - radio_ts(0) of the same frame, fitted against line 0's
// clock. At one site the true arrival difference is ~0, so the slope is the
// two receivers' crystal offset in ppm (groundwork for multilateration).
// Only DF17/18 pairs feed the fit (their payloads do not repeat within the
// window); a pair whose difference is more than `clock_reject_ticks` (plus
// 200 ppm of drift) from the last accepted pair's is rejected as a mismatch,
// and 64 rejections in a row restart the fit (a receiver restarted its
// counter, or the first pair was the bad one).
//
// Same-line repeats inside the window (a demodulator detecting a frame twice)
// are dropped too, counted separately; they are not "both lines" frames.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "adsb/common/histogram.hpp"
#include "adsb/record/recording.hpp"

namespace adsb::ingest {

inline constexpr int kMaxArbLines = 4;

struct ArbiterConfig {
  std::uint64_t window_ns = 5'000'000;       // 5 ms
  std::size_t max_entries = 65536;           // frames inside one window (rounded up to a power of two, <= 2^24)
  double radio_clock_hz = 12'000'000.0;      // Beast 12 MHz counter (for the clock fit's ns)
  double clock_reject_ticks = 1200.0;        // 100 us at 12 MHz
  std::uint64_t first_sequence = 0;          // testing only: where the internal entry counter starts
};

struct ArbLineStats {
  std::uint64_t received = 0;    // frames offered on this line
  std::uint64_t first = 0;       // forwarded: this line's copy came first
  std::uint64_t late = 0;        // dropped: another line already delivered it
  std::uint64_t same_line = 0;   // dropped: this line delivered it already (inside the window)
  std::uint64_t only = 0;        // frames no other line received (counted when they leave the window)
  std::uint64_t wins = 0;        // frames received by several lines where this line was first
};

// Linear fit of y = radio_ts(1) - radio_ts(0) (ticks) against x = line 0's
// radio_ts (ticks, unwrapped), streaming (Welford co-moments).
struct ClockFit {
  std::uint64_t n = 0;
  double y0 = 0;  // the first y: the fit works on y - y0, since y can be ~2^47
                  // ticks, where a double resolves only 1/32 tick
  double mean_x = 0, mean_y = 0, cxx = 0, cxy = 0, cyy = 0;

  void add(double x, double y_abs) noexcept {
    if (n == 0) y0 = y_abs;
    const double y = y_abs - y0;
    ++n;
    const double dx = x - mean_x;
    mean_x += dx / static_cast<double>(n);
    const double dy = y - mean_y;
    mean_y += dy / static_cast<double>(n);
    cxx += dx * (x - mean_x);
    cxy += dx * (y - mean_y);
    cyy += dy * (y - mean_y);
  }
  [[nodiscard]] double slope() const noexcept { return cxx > 0 ? cxy / cxx : 0.0; }
  // Offset (ticks) at x = x0.
  [[nodiscard]] double offset_at(double x0) const noexcept { return y0 + mean_y + slope() * (x0 - mean_x); }
  // RMS residual (ticks).
  [[nodiscard]] double residual_rms() const noexcept {
    if (n < 3 || cxx <= 0) return 0.0;
    const double ss = cyy - cxy * cxy / cxx;
    return ss > 0 ? std::sqrt(ss / static_cast<double>(n - 2)) : 0.0;
  }
};

struct ArbiterStats {
  std::uint64_t offered = 0;
  std::uint64_t forwarded = 0;
  std::uint64_t dropped = 0;          // late + same_line, all lines
  std::uint64_t multi = 0;            // frames received by two or more lines
  std::uint64_t unarbitrated = 0;     // line id >= kMaxArbLines or bad length: forwarded as is
  std::uint64_t evicted_early = 0;    // table full: an entry left before its window ended
  std::uint64_t time_backwards = 0;   // rx_ns earlier than the latest seen
  std::uint64_t clock_rejected = 0;   // clock pairs off the fitted line
  std::uint64_t clock_restarts = 0;   // fits restarted after 64 rejections in a row
  ArbLineStats line[kMaxArbLines];
  ClockFit clock01;                   // lines 0 and 1; x = 0 at the first frame both received
};

class Arbiter {
 public:
  explicit Arbiter(const ArbiterConfig& cfg = {})
      : cfg_(cfg),
        ring_mask_(std::bit_ceil(std::clamp<std::size_t>(cfg.max_entries, 16, std::size_t{1} << 24)) - 1),
        index_mask_((ring_mask_ + 1) * 2 - 1),
        ring_(ring_mask_ + 1),
        index_(index_mask_ + 1, kEmpty),
        margin_(std::make_unique<LatencyHistogram[]>(kMaxArbLines)),
        head_(cfg.first_sequence),
        tail_(cfg.first_sequence) {}

  // true = forward this frame; false = a copy already went out (dropped, counted).
  bool offer(const record::Record& r) noexcept {
    ++stats_.offered;
    if (r.line >= kMaxArbLines || r.len < 7 || r.len > 14) {
      ++stats_.unarbitrated;
      ++stats_.forwarded;
      return true;
    }
    std::uint64_t now = r.rx_ns;
    if (now < now_) {
      ++stats_.time_backwards;
      now = now_;
    }
    now_ = now;
    expire(now);
    auto& ls = stats_.line[r.line];
    ++ls.received;

    Key k = key_of(r);
    const std::size_t h = hash(k);
    for (std::size_t i = h & index_mask_;; i = (i + 1) & index_mask_) {
      const std::uint32_t e = index_[i];
      if (e == kEmpty) break;
      Entry& en = ring_[e & ring_mask_];
      if (en.k0 == k.k0 && en.k1 == k.k1) {
        on_copy(en, r, now, ls);
        ++stats_.dropped;
        return false;
      }
    }
    // First copy.
    if (count_ == ring_mask_ + 1) {
      ++stats_.evicted_early;
      retire_head();
    }
    const std::uint64_t seq = tail_++;
    Entry& en = ring_[seq & ring_mask_];
    en.k0 = k.k0;
    en.k1 = k.k1;
    en.rx_ns = now;
    en.radio_ts = r.radio_ts;
    en.first_line = r.line;
    en.seen = static_cast<std::uint8_t>(1u << r.line);
    // The index holds ring positions (< 2^24), never kEmpty.
    en.slot = insert_index(h, static_cast<std::uint32_t>(seq & ring_mask_));
    ++count_;
    ++ls.first;
    ++stats_.forwarded;
    return true;
  }

  // Ends every open window (end of input), so `only`/`multi` are complete.
  void finish() noexcept {
    while (count_ > 0) retire_head();
  }

  [[nodiscard]] const ArbiterStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const ArbiterConfig& config() const noexcept { return cfg_; }
  // Win margin when `line` was first: later copy's rx_ns - first copy's.
  [[nodiscard]] const LatencyHistogram& margin(int line) const noexcept {
    return margin_[static_cast<std::size_t>(line)];
  }
  [[nodiscard]] std::size_t open_entries() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return ring_mask_ + 1; }
  // Entries created so far, plus first_sequence (tests).
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return tail_; }

  // Line 1's radio clock relative to line 0's: rate offset in ppm.
  [[nodiscard]] double clock_ppm() const noexcept { return stats_.clock01.slope() * 1e6; }

 private:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
  static constexpr std::uint64_t kRadioMask = (std::uint64_t{1} << 48) - 1;

  struct Key {
    std::uint64_t k0 = 0, k1 = 0;
  };
  struct Entry {
    std::uint64_t k0 = 0, k1 = 0;
    std::uint64_t rx_ns = 0;
    std::uint64_t radio_ts = 0;
    std::size_t slot = 0;        // where the index points at this entry
    std::uint8_t first_line = 0;
    std::uint8_t seen = 0;       // bit per line that delivered a copy
  };

  static Key key_of(const record::Record& r) noexcept {
    std::uint8_t b[16] = {};
    std::memcpy(b, r.payload, r.len);
    b[15] = r.len;  // a 56-bit frame never equals a 112-bit one
    Key k;
    std::memcpy(&k.k0, b, 8);
    std::memcpy(&k.k1, b + 8, 8);
    return k;
  }
  static std::size_t hash(const Key& k) noexcept {
    // Mode S payloads end in a 24-bit CRC, so the bits are already well mixed;
    // a multiply folds both words into the high bits.
    const std::uint64_t x = (k.k0 ^ (k.k1 * 0x9E3779B97F4A7C15ULL)) * 0xD6E8FEB86659FD93ULL;
    return static_cast<std::size_t>(x >> 32);
  }

  std::size_t insert_index(std::size_t h, std::uint32_t seq32) noexcept {
    std::size_t i = h & index_mask_;
    while (index_[i] != kEmpty) i = (i + 1) & index_mask_;
    index_[i] = seq32;
    return i;
  }

  void on_copy(Entry& en, const record::Record& r, std::uint64_t now, ArbLineStats& ls) noexcept {
    const auto bit = static_cast<std::uint8_t>(1u << r.line);
    if (en.seen & bit) {
      ++ls.same_line;
      return;
    }
    ++ls.late;
    if (en.seen == (1u << en.first_line)) {  // second line to deliver it
      ++stats_.multi;
      ++stats_.line[en.first_line].wins;
    }
    en.seen = static_cast<std::uint8_t>(en.seen | bit);
    margin_[en.first_line].record(now - en.rx_ns);
    if ((en.first_line == 0 && r.line == 1) || (en.first_line == 1 && r.line == 0)) {
      const std::uint64_t ts0 = en.first_line == 0 ? en.radio_ts : r.radio_ts;
      const std::uint64_t ts1 = en.first_line == 0 ? r.radio_ts : en.radio_ts;
      const unsigned df = r.payload[0] >> 3;
      if (r.len == 14 && (df == 17 || df == 18)) add_clock(ts0, ts1);
    }
  }

  static double signed48(std::uint64_t d) noexcept {
    d &= kRadioMask;
    return d & (std::uint64_t{1} << 47) ? static_cast<double>(static_cast<std::int64_t>(d) -
                                                              (std::int64_t{1} << 48))
                                        : static_cast<double>(d);
  }

  void add_clock(std::uint64_t ts0, std::uint64_t ts1) noexcept {
    ClockFit& f = stats_.clock01;
    // Unwrap line 0's 48-bit counter and the difference: each advances by its
    // signed change since the last pair (so an offset near 2^47 is fine).
    const std::uint64_t d = (ts1 - ts0) & kRadioMask;
    double x = 0.0, y = signed48(d);
    if (f.n > 0) {
      x = x_unwrapped_ + signed48(ts0 - last_ts0_);
      y = y_unwrapped_ + signed48(d - last_d_);
    }
    // Two crystals drift apart by well under 200 ppm, so the difference
    // changes slowly; a pair far from the last accepted one is a mismatch
    // (independent of the fit, so an early bad pair cannot skew the test).
    if (f.n > 0 && std::abs(y - y_unwrapped_) > cfg_.clock_reject_ticks + 2e-4 * std::abs(x - x_unwrapped_)) {
      ++stats_.clock_rejected;
      if (++rejected_run_ < 64) return;  // keep x/y anchored at the last good pair
      ++stats_.clock_restarts;
      f = ClockFit{};
      x = 0.0;
      y = signed48(d);
    }
    rejected_run_ = 0;
    x_unwrapped_ = x;
    y_unwrapped_ = y;
    last_ts0_ = ts0;
    last_d_ = d;
    f.add(x, y);
  }

  void expire(std::uint64_t now) noexcept {
    while (count_ > 0) {
      const Entry& en = ring_[head_ & ring_mask_];
      if (now - en.rx_ns <= cfg_.window_ns) break;
      retire_head();
    }
  }

  void retire_head() noexcept {
    Entry& en = ring_[head_ & ring_mask_];
    if (std::popcount(static_cast<unsigned>(en.seen)) == 1) ++stats_.line[en.first_line].only;
    erase_index(en.slot);
    ++head_;
    --count_;
  }

  // Backward-shift deletion (no tombstones). Moving an index cell updates the
  // entry's `slot`.
  void erase_index(std::size_t i) noexcept {
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & index_mask_;
      const std::uint32_t e = index_[j];
      if (e == kEmpty) break;
      Entry& en = ring_[e & ring_mask_];
      const std::size_t home = hash(Key{en.k0, en.k1}) & index_mask_;
      // Move j to i if its home is not cyclically in (i, j].
      const bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
      if (stays) continue;
      index_[i] = e;
      en.slot = i;
      i = j;
    }
    index_[i] = kEmpty;
  }

  ArbiterConfig cfg_;
  std::size_t ring_mask_;
  std::size_t index_mask_;
  std::vector<Entry> ring_;            // entries in arrival order (a FIFO)
  std::vector<std::uint32_t> index_;   // open addressing: ring position of the entry
  std::unique_ptr<LatencyHistogram[]> margin_;
  std::uint64_t head_ = 0, tail_ = 0;
  std::size_t count_ = 0;
  std::uint64_t now_ = 0;
  std::uint64_t last_ts0_ = 0, last_d_ = 0;
  double x_unwrapped_ = 0.0, y_unwrapped_ = 0.0;
  int rejected_run_ = 0;
  ArbiterStats stats_;
};

// The report adsb-replay and adsb-live print (scripts parse these lines).
//   arbiter     window W us, offered N, forwarded F, dropped D, multi M, evicted early E, ...
//   arb line L  received R, first F, wins W, late L, same-line S, only O, margin p50/p99/max (us)
//   arb clock   line 1 vs 0: rate P ppm, offset X ticks, residual Y ns, n N
[[nodiscard]] inline std::string arbiter_report(const Arbiter& a) {
  const ArbiterStats& s = a.stats();
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::string out;
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "arbiter     window %llu us, table %zu, offered %llu, forwarded %llu, dropped %llu, multi %llu, "
                "evicted early %llu, time backwards %llu, unarbitrated %llu\n",
                u(a.config().window_ns / 1000), a.capacity(), u(s.offered), u(s.forwarded), u(s.dropped), u(s.multi),
                u(s.evicted_early), u(s.time_backwards), u(s.unarbitrated));
  out += buf;
  for (int l = 0; l < kMaxArbLines; ++l) {
    const ArbLineStats& ls = s.line[l];
    if (ls.received == 0) continue;
    const LatencyHistogram& m = a.margin(l);
    std::snprintf(buf, sizeof buf,
                  "arb line %d  received %llu, first %llu, wins %llu, late %llu, same-line %llu, only %llu, "
                  "margin p50 %.1f p99 %.1f max %.1f us\n",
                  l, u(ls.received), u(ls.first), u(ls.wins), u(ls.late), u(ls.same_line), u(ls.only),
                  m.count() ? static_cast<double>(m.percentile(50)) * 1e-3 : 0.0,
                  m.count() ? static_cast<double>(m.percentile(99)) * 1e-3 : 0.0,
                  m.count() ? static_cast<double>(m.max()) * 1e-3 : 0.0);
    out += buf;
  }
  const ClockFit& c = s.clock01;
  if (c.n >= 2) {
    std::snprintf(buf, sizeof buf,
                  "arb clock   line 1 vs 0: rate %+.3f ppm, offset %.0f ticks, residual %.1f ns, n %llu, "
                  "rejected %llu, restarts %llu\n",
                  a.clock_ppm(), c.offset_at(0.0), c.residual_rms() / a.config().radio_clock_hz * 1e9,
                  u(c.n), u(s.clock_rejected), u(s.clock_restarts));
    out += buf;
  }
  return out;
}

}  // namespace adsb::ingest
