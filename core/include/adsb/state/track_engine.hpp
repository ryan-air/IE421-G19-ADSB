#pragma once

// Track state engine (README_UPDATES.md §8).
//
// Consumes decoded frames in event-time order and maintains one track per
// aircraft. Emits protocol::TrackUpdate when a track's state changes and
// protocol::TrackDelete when it expires.
//
// Rules:
//  * Address/Parity frames (DF0/4/5/16/20/21) carry an address recovered from
//    the CRC, so a corrupted frame yields a random "address". They are only
//    applied to tracks already confirmed by a CRC-verified frame
//    (DF11/17/18); otherwise they are dropped and counted.
//  * Duplicates: a frame whose payload hash was seen on the same track within
//    `dedup_window_ns` is dropped (demodulator double-detections, or the
//    same transmission arriving on two receiver lines).
//  * Out-of-order: each field group (position, velocity, altitude, identity,
//    squawk, air/ground) keeps its own timestamp; an older update for that
//    group is ignored and counted.
//  * Positions: CPR global decoding from an even/odd pair within the pair
//    window, otherwise local decoding against the track's recent position
//    (or, for surface positions, the receiver). A position implying an
//    impossible speed is rejected, unless consecutive global decodes keep
//    disagreeing with the stored position, in which case the stored one is
//    assumed wrong and replaced.
//  * Expiry: a 64-second timing wheel driven by event time; a track with no
//    frames for `track_timeout_ns` is deleted.
//
// Everything is driven by the frames' own timestamps, never the wall clock,
// so replaying a recording produces identical output (§11.2).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#include "adsb/modes/decoder.hpp"
#include "adsb/protocol/feed_messages.hpp"
#include "adsb/state/track_store.hpp"

namespace adsb::state {

struct EngineConfig {
  bool has_receiver = false;
  double receiver_lat = 0.0;
  double receiver_lon = 0.0;

  std::uint64_t track_timeout_ns = 60'000'000'000ULL;
  std::uint64_t cpr_pair_window_ns = 10'000'000'000ULL;          // airborne
  std::uint64_t surface_pair_window_ns = 25'000'000'000ULL;
  std::uint64_t local_decode_max_age_ns = 60'000'000'000ULL;
  std::uint64_t dedup_window_ns = 2'000'000ULL;                   // 2 ms
  double max_speed_kt = 1000.0;          // airborne plausibility limit
  double max_surface_speed_kt = 200.0;
  // A position is replaced after this many consecutive rejected *global*
  // fixes: with 2, the third disagreeing global fix is accepted.
  std::uint8_t reanchor_after_rejects = 2;
  // Repair one wrong bit in DF17/18 frames (modes::DecodeOptions; readsb
  // does it by default). Off: a corrupted frame is dropped. A repaired
  // frame only updates an already confirmed track (it never creates one).
  bool fix_single_bit = false;
};

// Information about the frame that is not in DecodedMsg.
struct FrameMeta {
  std::uint64_t rx_ns = 0;
  std::uint64_t radio_ts = 0;  // 48-bit radio clock
  std::uint8_t line = 0;
  std::uint32_t payload_hash = 0;
};

struct EngineStats {
  std::uint64_t frames = 0;
  std::uint64_t bad_frames = 0;          // bad CRC / length / unknown format
  std::uint64_t corrected = 0;           // DF17/18 frames with one bit repaired (fix_single_bit)
  std::uint64_t corrected_rejected = 0;  // ... of which no confirmed track had the address: dropped
  std::uint64_t ap_accepted = 0;
  std::uint64_t ap_rejected = 0;         // Address/Parity with no confirmed track
  std::uint64_t duplicates = 0;
  std::uint64_t out_of_order = 0;        // field-group updates ignored as stale
  std::uint64_t cpr_global = 0;
  std::uint64_t cpr_local = 0;
  std::uint64_t cpr_unresolved = 0;      // no pair and no reference yet
  std::uint64_t cpr_rejected = 0;        // implausible jump
  std::uint64_t cpr_reanchored = 0;
  std::uint64_t table_full = 0;
  std::uint64_t tracks_created = 0;
  std::uint64_t tracks_expired = 0;
  std::uint64_t updates = 0;
  std::uint64_t deletes = 0;
};

// Hash of a frame's payload for duplicate detection (FNV-1a).
[[nodiscard]] inline std::uint32_t payload_hash(const std::uint8_t* p, std::size_t n) noexcept {
  std::uint32_t h = 2166136261u;
  for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

template <typename Store>
class BasicTrackEngine {
 public:
  explicit BasicTrackEngine(const EngineConfig& cfg = {}) : cfg_(cfg) { wheel_.fill(kNoTrack); }

  // Decodes and applies one raw frame.
  template <typename Sink>
  void on_frame(const std::uint8_t* frame, std::size_t len, const FrameMeta& meta, Sink& sink) {
    modes::DecodedMsg m;
    const modes::DecodeStatus st = modes::decode(frame, len, m, modes::DecodeOptions{cfg_.fix_single_bit});
    FrameMeta mm = meta;
    if (m.corrected_bit >= 0 && len == 14) {
      // Hash the repaired frame, so a clean copy of the same transmission
      // (another receiver line) and this one are duplicates of each other.
      std::uint8_t fixed[14];
      std::memcpy(fixed, frame, 14);
      fixed[m.corrected_bit / 8] = static_cast<std::uint8_t>(fixed[m.corrected_bit / 8] ^ (0x80u >> (m.corrected_bit % 8)));
      mm.payload_hash = payload_hash(fixed, len);
    } else {
      mm.payload_hash = payload_hash(frame, len);
    }
    on_decoded(st, m, mm, sink);
  }

  // Applies one already-decoded frame. `sink` must provide
  //   void on_update(const protocol::TrackUpdate&);
  //   void on_delete(const protocol::TrackDelete&);
  template <typename Sink>
  void on_decoded(modes::DecodeStatus st, const modes::DecodedMsg& m, const FrameMeta& meta, Sink& sink) {
    ++stats_.frames;
    if (m.corrected_bit >= 0) ++stats_.corrected;
    advance(meta.rx_ns, sink);

    TrackRef t;
    bool created = false;
    switch (st) {
      case modes::DecodeStatus::kOk:
        t = store_.find(m.icao);
        // A repaired frame is about as trustworthy as an Address/Parity one
        // (any frame one bit away from a valid one "passes": 107 times more
        // garbage gets through), so, as in readsb, it only updates a track a
        // clean frame already confirmed; it never creates or confirms one.
        if (m.corrected_bit >= 0 && (!t || !(t.hot->valid & kConfirmed))) {
          ++stats_.corrected_rejected;
          return;
        }
        if (!t) {
          t = store_.insert(m.icao);
          if (!t) {
            ++stats_.table_full;
            return;
          }
          created = true;
          ++stats_.tracks_created;
          t.hot->last_seen_ns = meta.rx_ns;
          wheel_insert(t);
        }
        t.hot->valid |= kConfirmed;
        break;
      case modes::DecodeStatus::kAddressParity:
        t = store_.find(m.icao);
        if (!t || !(t.hot->valid & kConfirmed)) {
          ++stats_.ap_rejected;
          return;
        }
        ++stats_.ap_accepted;
        break;
      default:
        ++stats_.bad_frames;
        return;
    }

    HotTrack& h = *t.hot;
    ColdTrack& c = *t.cold;
    if (!created && is_duplicate(c, meta)) {
      ++stats_.duplicates;
      return;
    }
    remember(c, meta);
    ++c.frames;
    if (meta.rx_ns > h.last_seen_ns) h.last_seen_ns = meta.rx_ns;

    std::uint16_t mask = created ? protocol::update_mask::kNewTrack : 0;
    apply_fields(h, c, m, meta.rx_ns, mask);
    if (m.has(modes::kFieldCpr)) apply_position(h, c, m, meta.rx_ns, mask);

    if (mask != 0) {
      c.radio_ts = (static_cast<std::uint64_t>(meta.line & 0xF) << 60) |
                   (meta.radio_ts & 0x0FFFFFFFFFFFFFFFULL);
      emit_update(h, c, meta.rx_ns, mask, sink);
    }
  }

  // Expires tracks idle for longer than the timeout, as of event time `now_ns`.
  // Called automatically by on_decoded; call it directly to flush at the end
  // of a stream or on a timer driven by event time.
  template <typename Sink>
  void advance(std::uint64_t now_ns, Sink& sink) {
    const std::uint64_t now_s = now_ns / 1'000'000'000ULL;
    if (!wheel_started_) {
      wheel_started_ = true;
      wheel_s_ = now_s;  // wheel_s_ = the next whole second to process
      return;
    }
    // Process every second that has fully elapsed. Bucket s % 64 holds tracks
    // whose expiry falls in second s; once now_s > s that second is entirely
    // in the past, so those tracks are expired exactly when due. After a gap
    // longer than one revolution, only the last 64 seconds need visiting:
    // every pending expiry lies in that window or later.
    if (now_s <= wheel_s_) return;
    if (now_s - wheel_s_ > kWheelSize) wheel_s_ = now_s - kWheelSize;
    while (wheel_s_ < now_s) {
      expire_bucket(static_cast<std::size_t>(wheel_s_ % kWheelSize), wheel_s_, sink);
      ++wheel_s_;
    }
  }

  [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] std::size_t track_count() const noexcept { return store_.size(); }
  [[nodiscard]] Store& store() noexcept { return store_; }

  // Builds the current TrackUpdate for a track (also used for snapshots).
  [[nodiscard]] static protocol::TrackUpdate make_update(const HotTrack& h, const ColdTrack& c,
                                                         std::uint64_t event_ts_ns,
                                                         std::uint16_t mask) noexcept {
    using protocol::TrackUpdate;
    TrackUpdate u;
    u.update_mask = mask;
    u.icao = h.icao;
    u.event_ts_ns = event_ts_ns;
    u.radio_ts = c.radio_ts;
    if (h.valid & kHasPosition) {
      u.lat_e7 = h.lat_e7;
      u.lon_e7 = h.lon_e7;
    }
    if (h.valid & kHasAltitude) u.alt_ft = h.alt_ft;
    if (h.valid & kHasGroundSpeed) u.gs_dkt = h.gs_dkt;
    if (h.valid & kHasTrack) u.track_cdeg = h.track_cdeg;
    if (h.valid & kHasVerticalRate) u.vrate_fpm = h.vrate_fpm;
    if (h.valid & kHasSquawk) u.squawk = h.squawk;
    if (h.valid & kHasCallsign) std::memcpy(u.callsign.data(), c.callsign, 8);
    if (h.valid & kHasCategory) u.category = h.category;
    if (h.valid & kHasHeading) u.heading_cdeg = c.heading_cdeg;
    if (h.valid & kHasAirspeed) u.airspeed_kt = c.airspeed_kt;
    if (h.valid & kHasPosition) {
      u.nic = c.nic;
      u.nuc_p = c.nuc_p;
    }
    u.adsb_version = c.adsb_version;
    u.nac_p = c.nac_p;
    u.sil = c.sil;
    u.flags = h.wire_flags;
    return u;
  }

 private:
  static constexpr std::size_t kWheelSize = 64;

  // ---- Duplicates ---------------------------------------------------------

  bool is_duplicate(const ColdTrack& c, const FrameMeta& meta) const noexcept {
    for (std::size_t i = 0; i < ColdTrack::kDedupEntries; ++i) {
      if (c.dedup_hash[i] == meta.payload_hash && c.dedup_ts[i] != 0) {
        const std::uint64_t a = c.dedup_ts[i];
        const std::uint64_t dt = meta.rx_ns > a ? meta.rx_ns - a : a - meta.rx_ns;
        if (dt <= cfg_.dedup_window_ns) return true;
      }
    }
    return false;
  }

  static void remember(ColdTrack& c, const FrameMeta& meta) noexcept {
    c.dedup_hash[c.dedup_next] = meta.payload_hash;
    c.dedup_ts[c.dedup_next] = meta.rx_ns == 0 ? 1 : meta.rx_ns;
    c.dedup_next = static_cast<std::uint8_t>((c.dedup_next + 1) % ColdTrack::kDedupEntries);
  }

  // ---- Non-position fields ------------------------------------------------

  // Returns true if an update stamped `rx` may overwrite a group stamped `ts`.
  bool fresh(std::uint64_t& ts, std::uint64_t rx) noexcept {
    if (rx < ts) {
      ++stats_.out_of_order;
      return false;
    }
    ts = rx;
    return true;
  }

  template <typename T>
  static void set_field(T& field, T value, std::uint16_t& valid, std::uint16_t valid_bit,
                        std::uint16_t& mask, std::uint16_t mask_bit) noexcept {
    if (!(valid & valid_bit) || field != value) {
      field = value;
      valid |= valid_bit;
      mask |= mask_bit;
    }
  }

  // Sets or clears a TrackFlags bit that qualifies a field, reporting a
  // change under that field's `mask_bit`. Callers set it only while the
  // field is known.
  static void set_flag(HotTrack& h, std::uint8_t flag, bool on, std::uint16_t& mask,
                       std::uint16_t mask_bit) noexcept {
    const std::uint8_t want = on ? flag : 0;
    if ((h.wire_flags & flag) == want) return;
    h.wire_flags = static_cast<std::uint8_t>((h.wire_flags & ~flag) | want);
    mask |= mask_bit;
  }

  static constexpr auto kAirData =
      static_cast<std::uint16_t>(protocol::update_mask::kHeading | protocol::update_mask::kAirspeed);
  static void drop_heading(HotTrack& h, std::uint16_t& mask) noexcept {
    h.valid = static_cast<std::uint16_t>(h.valid & ~kHasHeading);
    set_flag(h, protocol::track_flags::kHeadingTrue, false, mask, protocol::update_mask::kHeading);
    mask |= protocol::update_mask::kHeading;
  }
  static void drop_airspeed(HotTrack& h, std::uint16_t& mask) noexcept {
    h.valid = static_cast<std::uint16_t>(h.valid & ~kHasAirspeed);
    set_flag(h, protocol::track_flags::kAirspeedTrue, false, mask, protocol::update_mask::kAirspeed);
    mask |= protocol::update_mask::kAirspeed;
  }

  // NACp and SIL from the newest TC 29 / TC 31 (an older one arriving late
  // is ignored).
  static void set_accuracy(ColdTrack& c, std::uint8_t nac_p, std::uint8_t sil, bool per_sample, std::uint64_t rx,
                           std::uint16_t& mask) noexcept {
    if (rx < c.accuracy_ts_ns) return;
    c.accuracy_ts_ns = rx;
    const auto s = static_cast<std::uint8_t>((sil & 3u) | (per_sample ? 4u : 0u));
    if (c.nac_p != nac_p || c.sil != s) mask |= protocol::update_mask::kIntegrity;
    c.nac_p = nac_p;
    c.sil = s;
  }

  // Comm-B BDS 6,0 heading and indicated airspeed, for an aircraft without
  // ADS-B air data in the last 30 s. The register is inferred from its
  // contents (modes::decode_comm_b), so a value counts only when the
  // previous BDS 6,0 reply, at most 30 s earlier, agrees with it (heading
  // within 10 degrees, IAS within 20 kt).
  void apply_comm_b_air_data(HotTrack& h, ColdTrack& c, const modes::CommB& b, std::uint64_t rx,
                             std::uint16_t& mask) noexcept {
    namespace um = protocol::update_mask;
    namespace tf = protocol::track_flags;
    constexpr std::uint64_t kWindowNs = 30'000'000'000ULL;
    if (b.bds != 0x60 || !(b.valid & modes::CommB::kMagneticHeading) ||
        !(b.valid & modes::CommB::kIndicatedAirspeed))
      return;
    if (rx < c.commb_ts_ns) return;  // older than the last one
    const bool adsb = c.adsb_airdata_ts_ns != 0 && rx <= c.adsb_airdata_ts_ns + kWindowNs;
    const bool confirmed = c.commb_ts_ns != 0 && rx <= c.commb_ts_ns + kWindowNs && [&] {
      const float d = std::fabs(b.magnetic_heading_deg - c.commb_heading_deg);
      return std::min(d, 360.0f - d) <= 10.0f && std::fabs(b.indicated_airspeed_kt - c.commb_ias_kt) <= 20.0f;
    }();
    c.commb_ts_ns = rx;
    c.commb_heading_deg = b.magnetic_heading_deg;
    c.commb_ias_kt = b.indicated_airspeed_kt;
    if (adsb || !confirmed) return;
    auto hd = static_cast<std::uint16_t>(std::lround(static_cast<double>(b.magnetic_heading_deg) * 100.0));
    if (hd >= 36000) hd = 0;
    const auto as = static_cast<std::uint16_t>(
        std::min<long>(65534, std::lround(static_cast<double>(b.indicated_airspeed_kt))));
    set_field(c.heading_cdeg, hd, h.valid, kHasHeading, mask, um::kHeading);
    set_field(c.airspeed_kt, as, h.valid, kHasAirspeed, mask, um::kAirspeed);
    set_flag(h, tf::kHeadingTrue, false, mask, um::kHeading);  // magnetic
    set_flag(h, tf::kAirspeedTrue, false, mask, um::kAirspeed);  // indicated
    set_flag(h, tf::kAirDataCommB, true, mask, kAirData);
  }

  void apply_fields(HotTrack& h, ColdTrack& c, const modes::DecodedMsg& m, std::uint64_t rx,
                    std::uint16_t& mask) noexcept {
    namespace um = protocol::update_mask;
    namespace tf = protocol::track_flags;

    if (m.has(modes::kFieldAltitude) && fresh(h.alt_ts_ns, rx)) {
      set_field(h.alt_ft, m.altitude_ft, h.valid, kHasAltitude, mask, um::kAltitude);
      const std::uint8_t gnss = m.altitude_gnss ? tf::kAltitudeGnss : 0;
      if ((h.wire_flags & tf::kAltitudeGnss) != gnss) {
        h.wire_flags = static_cast<std::uint8_t>((h.wire_flags & ~tf::kAltitudeGnss) | gnss);
        mask |= um::kAltitude;
      }
    }

    const bool has_vel = m.has(modes::kFieldGroundSpeed) || m.has(modes::kFieldTrack) ||
                         m.has(modes::kFieldVerticalRate) || m.has(modes::kFieldHeading) ||
                         m.has(modes::kFieldAirspeed);
    if (has_vel && fresh(h.vel_ts_ns, rx)) {
      if (m.has(modes::kFieldGroundSpeed)) {
        const auto gs = static_cast<std::uint16_t>(
            std::min<long>(65534, std::lround(static_cast<double>(m.ground_speed_kt) * 10.0)));
        set_field(h.gs_dkt, gs, h.valid, kHasGroundSpeed, mask, um::kGroundSpeed);
      }
      if (m.has(modes::kFieldTrack)) {
        auto t = static_cast<std::uint16_t>(std::lround(static_cast<double>(m.track_deg) * 100.0));
        if (t >= 36000) t = 0;
        set_field(h.track_cdeg, t, h.valid, kHasTrack, mask, um::kTrack);
      }
      if (m.has(modes::kFieldVerticalRate)) {
        const auto vr = static_cast<std::int16_t>(std::clamp(m.vertical_rate_fpm, -32767, 32767));
        set_field(h.vrate_fpm, vr, h.valid, kHasVerticalRate, mask, um::kVerticalRate);
      }
      if (m.has(modes::kFieldHeading)) {
        auto hd = static_cast<std::uint16_t>(std::lround(static_cast<double>(m.heading_deg) * 100.0));
        if (hd >= 36000) hd = 0;
        set_field(c.heading_cdeg, hd, h.valid, kHasHeading, mask, um::kHeading);
        set_flag(h, tf::kHeadingTrue, c.heading_true_north, mask, um::kHeading);
        c.adsb_airdata_ts_ns = rx;
        // The airspeed was Comm-B's and this message has none: it is dropped,
        // since the flag can no longer say where it came from.
        if ((h.wire_flags & tf::kAirDataCommB) && !m.has(modes::kFieldAirspeed)) drop_airspeed(h, mask);
        set_flag(h, tf::kAirDataCommB, false, mask, kAirData);
      }
      if (m.has(modes::kFieldAirspeed)) {
        const auto as = static_cast<std::uint16_t>(
            std::min<long>(65534, std::lround(static_cast<double>(m.airspeed_kt))));
        set_field(c.airspeed_kt, as, h.valid, kHasAirspeed, mask, um::kAirspeed);
        set_flag(h, tf::kAirspeedTrue, m.airspeed_true, mask, um::kAirspeed);
        c.adsb_airdata_ts_ns = rx;
        if ((h.wire_flags & tf::kAirDataCommB) && !m.has(modes::kFieldHeading)) drop_heading(h, mask);
        set_flag(h, tf::kAirDataCommB, false, mask, kAirData);
      }
    }
    if (m.has(modes::kFieldCommB)) apply_comm_b_air_data(h, c, m.comm_b, rx, mask);

    // Operational status (TC 31) from an ADS-B source (DF17, or DF18 CF 0/1;
    // not TIS-B or ADS-R rebroadcasts): version, NIC supplements A and C,
    // and the heading reference. They apply to later positions only; the
    // current position's NIC was fixed at receipt, as in readsb.
    if (m.has(modes::kFieldOpStatus) && (m.df == 17 || (m.df == 18 && m.ca <= 1)) &&
        fresh(c.opstatus_ts_ns, rx)) {
      const modes::OperationalStatus& os = m.op_status;
      if (c.adsb_version != os.version) {
        c.adsb_version = os.version;
        mask |= um::kIntegrity;
      }
      // Version 0 has no NACp / SIL; 3-7 are reserved: a status of either
      // clears what an earlier version 1/2 status set.
      if (os.version != 1 && os.version != 2) set_accuracy(c, 0, 0, false, rx, mask);
      if (os.version == 1 || os.version == 2) {  // 3-7 are reserved
        c.nic_a = os.nic_supplement_a;
        c.nic_a_ts_ns = rx;
        if (os.nic_supplement_c_valid) {
          c.nic_c = os.nic_supplement_c;
          c.nic_c_ts_ns = rx;
        }
        // The wire flag shows the reference only once there is a heading
        // (and only for an ADS-B heading: Comm-B's is magnetic).
        c.heading_true_north = os.heading_true_north;
        if ((h.valid & kHasHeading) && !(h.wire_flags & tf::kAirDataCommB))
          set_flag(h, tf::kHeadingTrue, c.heading_true_north, mask, um::kHeading);
        // Version 1 has no SIL supplement: per hour.
        set_accuracy(c, os.nac_p, os.sil, os.version == 2 && os.sil_per_sample, rx, mask);
      }
    }
    // Target state and status (TC 29 subtype 1, version 2) also carries
    // NACp and SIL; same sources as TC 31.
    if (m.has(modes::kFieldTargetState) && (m.df == 17 || (m.df == 18 && m.ca <= 1))) {
      set_accuracy(c, m.target.nac_p, m.target.sil, m.target.sil_per_sample, rx, mask);
    }

    // Callsign and category arrive together in identification messages and
    // share one freshness timestamp.
    if ((m.has(modes::kFieldCallsign) || m.has(modes::kFieldCategory)) && fresh(c.ident_ts_ns, rx)) {
      if (m.has(modes::kFieldCallsign) &&
          (!(h.valid & kHasCallsign) || std::memcmp(c.callsign, m.callsign, 8) != 0)) {
        std::memcpy(c.callsign, m.callsign, 8);
        h.valid |= kHasCallsign;
        mask |= um::kCallsign;
      }
      if (m.has(modes::kFieldCategory) && m.category != 0) {
        set_field(h.category, m.category, h.valid, kHasCategory, mask, um::kCategory);
      }
    }
    if (m.has(modes::kFieldSquawk) && fresh(c.squawk_ts_ns, rx)) {
      set_field(h.squawk, m.squawk, h.valid, kHasSquawk, mask, um::kSquawk);
    }
    if (m.has(modes::kFieldAirGround) && fresh(c.ground_ts_ns, rx)) {
      const std::uint8_t g = m.on_ground ? tf::kOnGround : 0;
      if (!(h.valid & kHasAirGround) || (h.wire_flags & tf::kOnGround) != g) {
        h.wire_flags = static_cast<std::uint8_t>((h.wire_flags & ~tf::kOnGround) | g);
        h.valid |= kHasAirGround;
        mask |= um::kAirGround;
      }
    }
    if (m.non_icao_address) h.wire_flags |= tf::kNonIcaoAddress;
  }

  // ---- Positions ----------------------------------------------------------

  static double approx_distance_nm(double lat1, double lon1, double lat2, double lon2) noexcept {
    // Equirectangular approximation: accurate to well under 1% at the
    // distances that matter for a plausibility check.
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    double dlon = lon2 - lon1;
    if (dlon > 180.0) dlon -= 360.0;
    if (dlon < -180.0) dlon += 360.0;
    const double x = dlon * std::cos((lat1 + lat2) * 0.5 * kDeg);
    const double y = lat2 - lat1;
    return 60.0 * std::sqrt(x * x + y * y);
  }

  void apply_position(HotTrack& h, ColdTrack& c, const modes::DecodedMsg& m, std::uint64_t rx,
                      std::uint16_t& mask) noexcept {
    const bool surface = m.cpr_surface;
    CprSlot& slot = m.cpr.odd ? c.cpr_odd : c.cpr_even;
    const CprSlot& other = m.cpr.odd ? c.cpr_even : c.cpr_odd;
    // A position frame older than one already used is stale: applying it
    // would move the track backwards in time.
    if ((slot.valid && rx < slot.ts_ns) || (other.valid && rx < other.ts_ns) ||
        ((h.valid & kHasPosition) && rx < h.pos_ts_ns)) {
      ++stats_.out_of_order;
      return;
    }
    // Supplements expire like readsb's (TRACK_EXPIRE, 60 s after the last TC 31).
    constexpr std::uint64_t kSupplementTtlNs = 60'000'000'000ULL;
    const unsigned version = c.adsb_version == 0xFF ? 0u : c.adsb_version;
    const bool nic_a = c.nic_a && rx <= c.nic_a_ts_ns + kSupplementTtlNs;
    const bool nic_c = c.nic_c && rx <= c.nic_c_ts_ns + kSupplementTtlNs;
    slot = CprSlot{m.cpr, rx, true, surface, modes::position_nic(m.tc, version, nic_a, m.nic_supplement_b, nic_c),
                   (version == 0 || version > 2) ? modes::position_nuc(m.tc) : std::uint8_t{0}};

    const std::uint64_t window = surface ? cfg_.surface_pair_window_ns : cfg_.cpr_pair_window_ns;
    const bool have_pos = (h.valid & kHasPosition) != 0;
    const modes::LatLon prev{h.lat_e7 / 1e7, h.lon_e7 / 1e7};

    std::optional<modes::LatLon> fix;
    bool global = false;
    bool track_relative = false;  // local decode against the track's own position

    // 1. Global decode from an even/odd pair.
    if (other.valid && other.surface == surface && rx - other.ts_ns <= window) {
      const modes::CprFrame& even = m.cpr.odd ? other.frame : m.cpr;
      const modes::CprFrame& odd = m.cpr.odd ? m.cpr : other.frame;
      if (!surface) {
        fix = modes::cpr_global_airborne(even, odd, m.cpr.odd);
      } else if (have_pos || cfg_.has_receiver) {
        const modes::LatLon ref = have_pos ? prev : modes::LatLon{cfg_.receiver_lat, cfg_.receiver_lon};
        fix = modes::cpr_global_surface(even, odd, m.cpr.odd, ref);
      }
      global = fix.has_value();
    }
    // 2. Local decode against the track's recent position.
    if (!fix && have_pos && rx - h.pos_ts_ns <= cfg_.local_decode_max_age_ns &&
        ((h.wire_flags & protocol::track_flags::kSurfacePosition) != 0) == surface) {
      fix = surface ? modes::cpr_local_surface(m.cpr, prev) : modes::cpr_local_airborne(m.cpr, prev);
      track_relative = fix.has_value();
    }
    // 3. Surface positions only: local decode against the receiver.
    if (!fix && surface && cfg_.has_receiver) {
      fix = modes::cpr_local_surface(m.cpr, {cfg_.receiver_lat, cfg_.receiver_lon});
    }
    if (!fix) {
      ++stats_.cpr_unresolved;
      return;
    }

    // Plausibility against the previous accepted position.
    if (have_pos) {
      const double dist = approx_distance_nm(prev.lat, prev.lon, fix->lat, fix->lon);
      const double dt_h = static_cast<double>(rx > h.pos_ts_ns ? rx - h.pos_ts_ns : 0) / 3.6e12;
      const double vmax = surface ? cfg_.max_surface_speed_kt : cfg_.max_speed_kt;
      const double allowed = vmax * dt_h + 0.5;  // +0.5 NM for CPR / timing slack
      if (dist > allowed) {
        if (global && ++c.position_reject_streak > cfg_.reanchor_after_rejects) {
          ++stats_.cpr_reanchored;  // the stored position was the bad one
        } else {
          ++stats_.cpr_rejected;
          return;
        }
      }
    }
    c.position_reject_streak = 0;
    ++(global ? stats_.cpr_global : stats_.cpr_local);

    const auto lat_e7 = static_cast<std::int32_t>(std::lround(fix->lat * 1e7));
    const auto lon_e7 = static_cast<std::int32_t>(std::lround(fix->lon * 1e7));
    if (!have_pos || lat_e7 != h.lat_e7 || lon_e7 != h.lon_e7) mask |= protocol::update_mask::kPosition;
    // As in readsb: a global fix rests on both frames, a local one against
    // the track on this frame and the track's position (the weaker NIC
    // counts); a local one against the receiver on this frame alone.
    const std::uint8_t nic = global           ? std::min(slot.nic, other.nic)
                             : track_relative ? std::min(slot.nic, c.nic)
                                              : slot.nic;
    const std::uint8_t nuc = global           ? std::min(slot.nuc, other.nuc)
                             : track_relative ? std::min(slot.nuc, c.nuc_p)
                                              : slot.nuc;
    if (nic != c.nic || nuc != c.nuc_p) mask |= protocol::update_mask::kIntegrity;
    c.nic = nic;
    c.nuc_p = nuc;
    h.lat_e7 = lat_e7;
    h.lon_e7 = lon_e7;
    h.valid |= kHasPosition;
    h.pos_ts_ns = rx;
    namespace tf = protocol::track_flags;
    h.wire_flags = static_cast<std::uint8_t>(surface ? (h.wire_flags | tf::kSurfacePosition)
                                                     : (h.wire_flags & ~tf::kSurfacePosition));
  }

  // ---- Output -------------------------------------------------------------

  template <typename Sink>
  void emit_update(const HotTrack& h, const ColdTrack& c, std::uint64_t rx, std::uint16_t mask, Sink& sink) {
    ++stats_.updates;
    sink.on_update(make_update(h, c, rx, mask));
  }

  // ---- Expiry wheel -------------------------------------------------------

  std::uint64_t expiry_s(const HotTrack& h) const noexcept {
    return (h.last_seen_ns + cfg_.track_timeout_ns) / 1'000'000'000ULL;
  }

  void wheel_insert(TrackRef t) noexcept {
    // A track created by an out-of-order frame can already be due; file it
    // under the next second to process instead of a bucket already passed
    // (which would not be visited again for a full revolution).
    std::uint64_t due = expiry_s(*t.hot);
    if (wheel_started_ && due < wheel_s_) due = wheel_s_;
    const std::size_t b = static_cast<std::size_t>(due % kWheelSize);
    t.cold->wheel_next = wheel_[b];
    wheel_[b] = t.hot->icao;
  }

  template <typename Sink>
  void expire_bucket(std::size_t bucket, std::uint64_t second, Sink& sink) {
    std::uint32_t icao = wheel_[bucket];
    wheel_[bucket] = kNoTrack;
    while (icao != kNoTrack) {
      TrackRef t = store_.find(icao);
      if (!t) break;  // cannot happen: tracks leave the store only here
      const std::uint32_t next = t.cold->wheel_next;
      if (expiry_s(*t.hot) <= second) {
        protocol::TrackDelete d;
        d.reason = 1;
        d.icao = icao;
        // Event time at which the track timed out (not the frame that
        // happened to trigger the check), so the value is the same however
        // often advance() is called.
        d.event_ts_ns = t.hot->last_seen_ns + cfg_.track_timeout_ns;
        store_.erase(icao);
        ++stats_.tracks_expired;
        ++stats_.deletes;
        sink.on_delete(d);
      } else {
        wheel_insert(t);  // seen since it was queued: re-file under its new expiry
      }
      icao = next;
    }
  }

  EngineConfig cfg_;
  Store store_;
  EngineStats stats_;
  std::array<std::uint32_t, kWheelSize> wheel_{};
  bool wheel_started_ = false;
  std::uint64_t wheel_s_ = 0;
};

using TrackEngine = BasicTrackEngine<FlatTrackStore<8192>>;
using NaiveTrackEngine = BasicTrackEngine<NaiveTrackStore>;

}  // namespace adsb::state
