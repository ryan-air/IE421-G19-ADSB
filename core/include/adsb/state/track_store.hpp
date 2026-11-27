#pragma once

// Per-aircraft track records and the stores that hold them
// (README_UPDATES.md §8.1).
//
// A track is split into
//  * HotTrack  — exactly one 64-byte cache line: everything a TrackUpdate is
//                built from and every timestamp checked on each frame;
//  * ColdTrack — callsign, CPR frame buffers, duplicate filter, wheel link;
//                touched only by the messages that need it.
//
// Two interchangeable stores share one interface:
//  * FlatTrackStore  — open addressing keyed by the 24-bit ICAO address,
//                      linear probing, Fibonacci hashing, backward-shift
//                      deletion (no tombstones), fixed capacity, all memory
//                      allocated in the constructor;
//  * NaiveTrackStore — std::unordered_map<icao, record>: the baseline the
//                      flat store is benchmarked against (allocates on insert).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "adsb/common/cache.hpp"
#include "adsb/modes/cpr.hpp"

namespace adsb::state {

// Internal validity bits for HotTrack::valid.
enum TrackValid : std::uint16_t {
  kHasPosition = 1u << 0,
  kHasAltitude = 1u << 1,
  kHasGroundSpeed = 1u << 2,
  kHasTrack = 1u << 3,
  kHasVerticalRate = 1u << 4,
  kHasCallsign = 1u << 5,
  kHasSquawk = 1u << 6,
  kHasCategory = 1u << 7,
  kHasAirGround = 1u << 8,
  kHasHeading = 1u << 9,
  kHasAirspeed = 1u << 10,
  kConfirmed = 1u << 15,  // seen in at least one CRC-verified frame
};

struct alignas(64) HotTrack {
  std::uint32_t icao = 0;
  std::uint16_t valid = 0;       // TrackValid bits
  std::uint8_t wire_flags = 0;   // protocol::track_flags bits
  std::uint8_t category = 0;
  std::int32_t lat_e7 = 0;
  std::int32_t lon_e7 = 0;
  std::int32_t alt_ft = 0;
  std::uint16_t gs_dkt = 0;
  std::uint16_t track_cdeg = 0;
  std::int16_t vrate_fpm = 0;
  std::uint16_t squawk = 0;
  std::uint64_t last_seen_ns = 0;  // newest frame of any kind
  std::uint64_t pos_ts_ns = 0;     // newest accepted position
  std::uint64_t vel_ts_ns = 0;     // newest velocity / surface movement
  std::uint64_t alt_ts_ns = 0;     // newest accepted altitude
};
static_assert(sizeof(HotTrack) == 64, "HotTrack must be exactly one cache line");

struct CprSlot {
  modes::CprFrame frame;
  std::uint64_t ts_ns = 0;
  bool valid = false;
  bool surface = false;
  std::uint8_t nic = 0;  // of this frame (modes::position_nic at receipt, as readsb)
  std::uint8_t nuc = 0;  // of this frame (modes::position_nuc), version 0 aircraft only
};

struct ColdTrack {
  char callsign[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  std::uint64_t ident_ts_ns = 0;
  std::uint64_t squawk_ts_ns = 0;
  std::uint64_t ground_ts_ns = 0;
  std::uint64_t radio_ts = 0;     // of the newest frame that produced an update; line id in the top bits
  // Airborne velocity subtypes 3/4 (share HotTrack::vel_ts_ns).
  std::uint16_t heading_cdeg = 0;
  std::uint16_t airspeed_kt = 0;
  // Integrity: operational status (TC 31) and the current position's NIC.
  std::uint64_t opstatus_ts_ns = 0;
  std::uint8_t adsb_version = 0xFF;  // 0xFF = no TC 31 yet (positions use version 0 rules)
  bool nic_a = false;
  bool nic_c = false;
  bool heading_true_north = false;   // HRD of the latest version 1/2 TC 31
  std::uint8_t nic = 0;
  std::uint64_t nic_a_ts_ns = 0;     // NIC-A / NIC-C count for 60 s after the TC 31 that set them
  std::uint64_t nic_c_ts_ns = 0;
  std::uint8_t nuc_p = 0;            // current position, version 0 aircraft
  std::uint8_t nac_p = 0;            // latest TC 31 (v1/2) or TC 29
  std::uint8_t sil = 0;              // wire byte: SIL bits 0-1, per-sample bit 2
  std::uint64_t accuracy_ts_ns = 0;  // newest TC 29 / TC 31 that set nac_p / sil
  // Air data (heading, airspeed): from ADS-B velocity subtypes 3/4, or, for
  // aircraft without them for 30 s, Comm-B BDS 6,0 confirmed by two agreeing
  // replies (the register is inferred, so one reply could be another one's).
  std::uint64_t adsb_airdata_ts_ns = 0;  // newest subtype 3/4 heading or airspeed
  std::uint64_t commb_ts_ns = 0;         // newest BDS 6,0 candidate
  float commb_heading_deg = 0.0f;
  float commb_ias_kt = 0.0f;
  CprSlot cpr_even;
  CprSlot cpr_odd;
  std::uint8_t position_reject_streak = 0;
  // Altitude plausibility (readsb's alt_reliable, 0-20) and the source of
  // the stored altitude (TrackEngine AltSource; 0 = none).
  std::int8_t alt_reliable = 0;
  std::uint8_t alt_source = 0;
  std::uint8_t ground_source = 0;  // of the air/ground state: 0 none, 1 Address/Parity reply, 2 CRC-checked frame
  std::uint64_t vrate_ts_ns = 0;  // newest vertical rate (HotTrack::vel_ts_ns also moves on surface movement etc.)
  // Duplicate filter: the last few frames' payload hashes.
  static constexpr std::size_t kDedupEntries = 4;
  std::uint32_t dedup_hash[kDedupEntries] = {};
  std::uint64_t dedup_ts[kDedupEntries] = {};
  std::uint8_t dedup_next = 0;
  // Expiry timing wheel: next ICAO in the same bucket (kNoTrack = end).
  std::uint32_t wheel_next = 0;
  std::uint64_t frames = 0;
};

inline constexpr std::uint32_t kNoTrack = 0xFFFFFFFFu;  // not a valid 24-bit address

struct TrackRef {
  HotTrack* hot = nullptr;
  ColdTrack* cold = nullptr;
  explicit operator bool() const noexcept { return hot != nullptr; }
};

// ---------------------------------------------------------------------------
// Flat open-addressing store
// ---------------------------------------------------------------------------

template <std::size_t Capacity = 8192>
class FlatTrackStore {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

 public:
  // The index has 2x as many entries as there are track slots, so the load
  // factor never exceeds 0.5 and probe sequences stay short.
  static constexpr std::size_t kIndexSize = 2 * Capacity;

  // All storage is allocated here, once; nothing allocates afterwards.
  FlatTrackStore() : index_(kIndexSize), hot_(Capacity), cold_(Capacity), free_(Capacity) { clear(); }

  void clear() noexcept {
    for (std::size_t i = 0; i < kIndexSize; ++i) index_[i] = IndexEntry{kNoTrack, 0};
    for (std::size_t i = 0; i < Capacity; ++i) {
      free_[i] = static_cast<std::uint32_t>(Capacity - 1 - i);
      hot_[i] = HotTrack{};
      cold_[i] = ColdTrack{};
    }
    free_top_ = Capacity;
    size_ = 0;
  }

  [[nodiscard]] TrackRef find(std::uint32_t icao) noexcept {
    std::size_t i = home(icao);
    for (;;) {
      const IndexEntry& e = index_[i];
      if (e.key == icao) return ref(e.slot);
      if (e.key == kNoTrack) return {};
      i = (i + 1) & kIndexMask;
    }
  }

  // Precondition: icao is not present. Returns an empty ref when full.
  [[nodiscard]] TrackRef insert(std::uint32_t icao) noexcept {
    if (free_top_ == 0) return {};
    const std::uint32_t slot = free_[--free_top_];
    std::size_t i = home(icao);
    while (index_[i].key != kNoTrack) i = (i + 1) & kIndexMask;
    index_[i] = IndexEntry{icao, slot};
    hot_[slot] = HotTrack{};
    cold_[slot] = ColdTrack{};
    hot_[slot].icao = icao;
    ++size_;
    return ref(slot);
  }

  bool erase(std::uint32_t icao) noexcept {
    std::size_t i = home(icao);
    for (;;) {
      if (index_[i].key == icao) break;
      if (index_[i].key == kNoTrack) return false;
      i = (i + 1) & kIndexMask;
    }
    free_[free_top_++] = index_[i].slot;
    --size_;
    // Backward-shift deletion: pull later entries of the probe chain into the
    // hole unless that would move them before their home bucket.
    std::size_t hole = i;
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & kIndexMask;
      if (index_[j].key == kNoTrack) break;
      const std::size_t h = home(index_[j].key);
      // Entry j may move into `hole` only if its home is not in (hole, j].
      const bool home_in_between = (hole <= j) ? (hole < h && h <= j) : (hole < h || h <= j);
      if (home_in_between) continue;
      index_[hole] = index_[j];
      hole = j;
    }
    index_[hole] = IndexEntry{kNoTrack, 0};
    return true;
  }

  template <typename Fn>
  void for_each(Fn&& fn) {
    for (std::size_t i = 0; i < kIndexSize; ++i) {
      if (index_[i].key != kNoTrack) fn(ref(index_[i].slot));
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // Longest probe distance currently in the index (for tests / diagnostics).
  [[nodiscard]] std::size_t max_probe_length() const noexcept {
    std::size_t worst = 0;
    for (std::size_t i = 0; i < kIndexSize; ++i) {
      if (index_[i].key == kNoTrack) continue;
      const std::size_t d = (i - home(index_[i].key)) & kIndexMask;
      if (d > worst) worst = d;
    }
    return worst;
  }

 private:
  struct IndexEntry {
    std::uint32_t key;
    std::uint32_t slot;
  };
  static constexpr std::size_t kIndexMask = kIndexSize - 1;
  static constexpr unsigned kIndexBits = [] {
    unsigned b = 0;
    while ((std::size_t{1} << b) < kIndexSize) ++b;
    return b;
  }();

  // Fibonacci hashing: multiply by 2^32 / phi and keep the top bits. ICAO
  // addresses are allocated in country blocks, so their low bits alone are a
  // poor hash; the multiply mixes high bits into the result.
  static std::size_t home(std::uint32_t icao) noexcept {
    return static_cast<std::size_t>((icao * 0x9E3779B1u) >> (32 - kIndexBits));
  }

  TrackRef ref(std::uint32_t slot) noexcept { return {&hot_[slot], &cold_[slot]}; }

  std::vector<IndexEntry> index_;
  std::vector<HotTrack> hot_;
  std::vector<ColdTrack> cold_;
  std::vector<std::uint32_t> free_;
  std::size_t free_top_ = 0;
  std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Baseline: std::unordered_map
// ---------------------------------------------------------------------------

class NaiveTrackStore {
 public:
  explicit NaiveTrackStore(std::size_t capacity = 8192) : capacity_(capacity) {}

  [[nodiscard]] TrackRef find(std::uint32_t icao) {
    const auto it = map_.find(icao);
    return it == map_.end() ? TrackRef{} : TrackRef{&it->second.hot, &it->second.cold};
  }

  [[nodiscard]] TrackRef insert(std::uint32_t icao) {
    if (map_.size() >= capacity_) return {};
    auto& rec = map_[icao];  // allocates a node
    rec = Record{};
    rec.hot.icao = icao;
    return {&rec.hot, &rec.cold};
  }

  bool erase(std::uint32_t icao) { return map_.erase(icao) != 0; }

  template <typename Fn>
  void for_each(Fn&& fn) {
    for (auto& [icao, rec] : map_) fn(TrackRef{&rec.hot, &rec.cold});
  }

  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  void clear() { map_.clear(); }

 private:
  struct Record {
    HotTrack hot;
    ColdTrack cold;
  };
  std::unordered_map<std::uint32_t, Record> map_;
  std::size_t capacity_;
};

}  // namespace adsb::state
