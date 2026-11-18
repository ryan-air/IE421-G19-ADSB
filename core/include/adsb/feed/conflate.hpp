#pragma once

// Conflator: builds the conflated feed (README_UPDATES.md §10.2) — only the
// latest state of each aircraft, at a fixed rate — for slow consumers such as
// the browser gateway and camera fusion.
//
// It sits beside the main publisher as a second track-engine sink:
//
//   engine --on_update/on_delete--> main Publisher (every update)
//          \-------------------->  Conflator --tick every interval--> conflated Publisher
//
// Between ticks it only records: the latest TrackUpdate per aircraft, the
// union of the update masks it has not sent yet, and whether the aircraft was
// deleted. On each tick it sends, for every aircraft that changed since the
// last tick (and only those, so the cost is proportional to the changes):
//   * a TrackUpdate with the latest state and update_mask = everything that
//     changed since the aircraft was last sent;
//   * or, if it was deleted, a TrackDelete (nothing at all if it was created
//     and deleted within one tick and so never sent).
//
// Invariant (tested): right after every tick, the book a subscriber builds
// from the conflated feed holds exactly the main feed's aircraft with exactly
// the same state; only update_mask (a union) and publish_ts_ns (stamped by the
// conflated publisher) differ. FeedBook::state_digest() compares that.
//
// Hot path: on_update / on_delete do one hash lookup and a 64-byte copy, no
// allocation (the table and the dirty list are sized at construction). If the
// table is ever full, updates for aircraft it cannot hold pass straight
// through, unconflated and counted: state is never lost. (The table also
// holds deleted aircraft until the next tick, so size it above the number of
// live aircraft.)
//
// Time is the caller's: poll(now_ns) ticks when now_ns has passed the next
// tick. Replay passes event time (deterministic); live passes mono_ns().

#include <cstddef>
#include <cstdint>
#include <vector>

#include "adsb/protocol/feed_messages.hpp"

namespace adsb::feed {

struct ConflatorConfig {
  std::uint64_t interval_ns = 100'000'000;  // 10 Hz
  std::size_t capacity = 16384;             // aircraft tracked at once (rounded up to a power of two)
};

struct ConflatorStats {
  std::uint64_t updates_in = 0;
  std::uint64_t deletes_in = 0;
  std::uint64_t updates_out = 0;
  std::uint64_t deletes_out = 0;
  std::uint64_t suppressed = 0;    // created and deleted within one tick: never sent
  std::uint64_t passthrough = 0;   // table full: sent unconflated
  std::uint64_t ticks = 0;
};

// `Out` must provide on_update(const protocol::TrackUpdate&) and
// on_delete(const protocol::TrackDelete&) (a Publisher does).
template <typename Out>
class Conflator {
 public:
  Conflator(const ConflatorConfig& cfg, Out& out) : cfg_(cfg), out_(out) {
    std::size_t cap = 2;
    while (cap < cfg_.capacity * 2) cap <<= 1;  // load factor <= 0.5
    slots_.resize(cap);
    mask_ = cap - 1;
    dirty_.reserve(cap);
    if (cfg_.interval_ns == 0) cfg_.interval_ns = 1;
  }

  // ---- Track-engine sink --------------------------------------------------
  void on_update(const protocol::TrackUpdate& u) {
    ++stats_.updates_in;
    Slot* s = find_or_insert(u.icao);
    if (s == nullptr) {  // table full: never lose state
      ++stats_.passthrough;
      overflowed_ = true;
      out_.on_update(u);
      return;
    }
    s->last = u;
    s->pending_mask = static_cast<std::uint16_t>(s->pending_mask | u.update_mask);
    s->deleted = false;  // deleted then re-created within the tick: send the latest state
    mark_dirty(*s);
  }

  void on_delete(const protocol::TrackDelete& d) {
    ++stats_.deletes_in;
    Slot* s = find(d.icao);
    if (s == nullptr) {
      // Not tracked (its updates passed through, or it was never seen): pass
      // the delete through too, so subscribers do not keep a stale aircraft.
      ++stats_.passthrough;
      out_.on_delete(d);
      return;
    }
    s->deleted = true;
    s->del = d;
    mark_dirty(*s);
  }

  // ---- Ticks ----------------------------------------------------------------
  // Sends the changes if `now_ns` has reached the next tick. Returns true if it ticked.
  bool poll(std::uint64_t now_ns) {
    if (next_tick_ns_ == 0) next_tick_ns_ = now_ns + cfg_.interval_ns;
    if (now_ns < next_tick_ns_) return false;
    tick();
    // Next tick one interval later; if we fell behind by more, restart from now
    // rather than firing a burst of empty ticks.
    next_tick_ns_ += cfg_.interval_ns;
    if (next_tick_ns_ <= now_ns) next_tick_ns_ = now_ns + cfg_.interval_ns;
    return true;
  }

  // Sends every pending change now (also used at the end of a session).
  void tick() {
    ++stats_.ticks;
    for (const std::uint32_t icao : dirty_) {
      Slot* s = find(icao);
      if (s == nullptr) continue;  // (cannot happen: dirty slots are erased only here)
      s->dirty = false;
      if (s->deleted) {
        // "Never sent" is only known for sure while nothing has passed
        // through: an aircraft whose updates passed through (table full) and
        // that later got a slot looks unsent, yet subscribers have it. After
        // any overflow, send every delete (a delete of an unknown aircraft is
        // harmless to a subscriber).
        if (s->sent || overflowed_) {
          out_.on_delete(s->del);
          ++stats_.deletes_out;
        } else {
          ++stats_.suppressed;
        }
        erase(icao);
        continue;
      }
      protocol::TrackUpdate m = s->last;
      m.update_mask = s->pending_mask;
      out_.on_update(m);
      ++stats_.updates_out;
      s->sent = true;
      s->pending_mask = 0;
    }
    dirty_.clear();
  }

  [[nodiscard]] const ConflatorStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t pending() const noexcept { return dirty_.size(); }
  [[nodiscard]] std::size_t tracked() const noexcept { return size_; }
  [[nodiscard]] std::uint64_t interval_ns() const noexcept { return cfg_.interval_ns; }

 private:
  struct Slot {
    std::uint32_t icao = 0;
    bool used = false;
    bool dirty = false;
    bool sent = false;     // the conflated feed has carried this aircraft
    bool deleted = false;  // a delete is pending
    std::uint16_t pending_mask = 0;
    protocol::TrackUpdate last{};
    protocol::TrackDelete del{};
  };

  [[nodiscard]] std::size_t home(std::uint32_t icao) const noexcept {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(icao) * 0x9E3779B97F4A7C15ULL) >> 32) & mask_;
  }

  Slot* find(std::uint32_t icao) noexcept {
    for (std::size_t i = home(icao);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (!s.used) return nullptr;
      if (s.icao == icao) return &s;
    }
  }

  Slot* find_or_insert(std::uint32_t icao) noexcept {
    for (std::size_t i = home(icao);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.used && s.icao == icao) return &s;
      if (!s.used) {
        if (size_ >= cfg_.capacity) return nullptr;
        s = Slot{};
        s.used = true;
        s.icao = icao;
        ++size_;
        return &s;
      }
    }
  }

  void mark_dirty(Slot& s) {
    if (s.dirty) return;
    s.dirty = true;
    dirty_.push_back(s.icao);  // reserved at construction: never reallocates
  }

  // Backward-shift deletion (no tombstones), as in FlatTrackStore.
  void erase(std::uint32_t icao) noexcept {
    std::size_t i = home(icao);
    while (slots_[i].used && slots_[i].icao != icao) i = (i + 1) & mask_;
    if (!slots_[i].used) return;
    slots_[i].used = false;
    --size_;
    for (std::size_t j = (i + 1) & mask_; slots_[j].used; j = (j + 1) & mask_) {
      const std::size_t h = home(slots_[j].icao);
      // Move slot j back into the hole at i if its home is not in (i, j].
      const bool in_range = i <= j ? (h > i && h <= j) : (h > i || h <= j);
      if (!in_range) {
        slots_[i] = slots_[j];
        slots_[j].used = false;
        i = j;
      }
    }
  }

  ConflatorConfig cfg_;
  Out& out_;
  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  std::vector<std::uint32_t> dirty_;
  std::uint64_t next_tick_ns_ = 0;
  bool overflowed_ = false;  // something has passed through (see tick())
  ConflatorStats stats_;
};

}  // namespace adsb::feed
