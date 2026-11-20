#include "adsb/feed/dst.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <unordered_map>

#include "adsb/common/rng.hpp"
#include "adsb/feed/book.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"
#include "adsb/feed/snapshot.hpp"
#include "adsb/protocol/feed_messages.hpp"
#include "adsb/protocol/moldudp64.hpp"

namespace adsb::feed::dst {

void Totals::add(const Totals& o) {
  published += o.published;
  sessions += o.sessions;
  datagrams += o.datagrams;
  lost += o.lost;
  duplicated += o.duplicated;
  queue_overflow += o.queue_overflow;
  stalls += o.stalls;
  forced_drains += o.forced_drains;
  publisher_blocked += o.publisher_blocked;
  applied += o.applied;
  gaps += o.gaps;
  requests += o.requests;
  request_retries += o.request_retries;
  naks += o.naks;
  snapshots_loaded += o.snapshots_loaded;
  snapshot_failures += o.snapshot_failures;
  session_changes += o.session_changes;
  stale_packets += o.stale_packets;
  window_overflow += o.window_overflow;
  truncated_replies += o.truncated_replies;
  duplicates += o.duplicates;
  checks += o.checks;
}

namespace {

// The simulated clock. thread_local: a sweep runs simulations on several
// threads, one at a time per thread. Publisher takes a plain function.
thread_local std::uint64_t t_now = 0;
std::uint64_t sim_clock() { return t_now; }

using Bytes = std::vector<std::uint8_t>;
using Buf = std::shared_ptr<const Bytes>;
using Source = SubscriberCore::Source;

enum class Kind : std::uint8_t {
  kPublish,       // the engine emits messages
  kPubPoll,       // the publisher's loop polls (batches, heartbeats)
  kRecovery,      // one pass of the recovery thread's loop
  kRequest,       // a retransmit request reaches the server
  kFeed,          // a feed datagram reaches a subscriber
  kReply,         // a retransmit reply reaches a subscriber
  kSubTick,       // a subscriber's loop wakes up (about every millisecond)
  kSnapServe,     // the snapshot server accepts a connection
  kSnapDone,      // the snapshot fetch completes (or fails)
  kRestart,       // the publisher crashes or ends cleanly
  kUp,            // ... and comes back with a new session
  kJoin,          // a subscriber subscribes
  kChaosEnd,      // faults stop, the publisher ends its session
};

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::kPublish: return "publish";
    case Kind::kPubPoll: return "pub-poll";
    case Kind::kRecovery: return "recovery";
    case Kind::kRequest: return "request";
    case Kind::kFeed: return "feed";
    case Kind::kReply: return "reply";
    case Kind::kSubTick: return "sub-tick";
    case Kind::kSnapServe: return "snap-serve";
    case Kind::kSnapDone: return "snap-done";
    case Kind::kRestart: return "restart";
    case Kind::kUp: return "up";
    case Kind::kJoin: return "join";
    case Kind::kChaosEnd: return "chaos-end";
  }
  return "?";
}

struct Event {
  std::uint64_t t = 0;
  std::uint64_t order = 0;  // ties: scheduling order
  Kind kind = Kind::kPublish;
  int sub = -1;
  std::uint64_t aux = 0;    // era or fetch id
  Buf data;
};
struct Later {
  bool operator()(const Event& a, const Event& b) const noexcept {
    return a.t != b.t ? a.t > b.t : a.order > b.order;
  }
};

struct World;

struct SimOut {
  World* w = nullptr;
  bool send(std::span<const std::uint8_t> d) noexcept;
};

// One publisher process lifetime: a session.
struct Era {
  protocol::Session session{};
  std::unique_ptr<MessageLog> log;
  std::unique_ptr<RecoveryState> rec;
  std::unique_ptr<Publisher<SimOut>> pub;
  bool alive = true;
  // The truth: every message published, and the book's fingerprint after
  // each one.
  Bytes bytes;
  std::vector<std::uint32_t> off{0};
  std::vector<std::uint64_t> fp_at{0};
  std::unordered_map<std::uint32_t, std::uint64_t> track_fp;
  std::uint64_t fp = 0;
  FeedBook truth;

  [[nodiscard]] std::uint64_t count() const noexcept { return off.size() - 1; }
  [[nodiscard]] std::span<const std::uint8_t> msg(std::uint64_t seq) const noexcept {
    return {bytes.data() + off[seq - 1], off[seq] - off[seq - 1]};
  }
};

struct LinkState {
  std::uint32_t burst_left = 0;
};

struct Sub final : SubscriberIo, SubscriberHandler {
  World* w = nullptr;
  int idx = 0;
  SubscriberSpec spec;
  std::unique_ptr<SubscriberCore> core;
  bool joined = false;
  LinkState feed_ls, req_ls, rep_ls;
  // Snapshot fetch.
  bool fetching = false;
  std::uint64_t fetch_id = 0;
  std::optional<SnapshotData> fetched;  // what the server sent, while in transfer
  int fetched_era = -1;
  bool flushed_if_dead = false;  // a crashed server's kernel still delivers the stream
  std::deque<std::pair<Source, Buf>> queue;  // kernel queue during a blocking fetch
  // Checks.
  bool fresh = true;
  std::uint64_t last_seq = 0;
  int max_era = -1;  // newest session it has been live in
  std::vector<int> left;  // sessions it has been in and left
  int chk_era = -2;
  std::uint64_t chk_n = ~std::uint64_t{0};
  bool dirty = true;

  void send_request(const protocol::RetransmitRequest& req) override;
  void on_message(std::uint64_t seq, std::span<const std::uint8_t> msg) override;
  void on_reset() override {
    fresh = true;
    dirty = true;
  }
  void on_snapshot_loaded() override {
    fresh = true;
    dirty = true;
  }
};

struct World {
  const Config& cfg;
  Sabotage sabotage;
  const TraceFn* trace;
  Rng rng;
  std::priority_queue<Event, std::vector<Event>, Later> q;
  std::uint64_t order = 0;
  SimOut out;
  std::vector<std::unique_ptr<Era>> eras;
  int cur = -1;     // the era of the running (or last) publisher
  bool up = false;  // a publisher process is running
  std::vector<std::unique_ptr<Sub>> subs;
  bool chaos = true;
  bool publishing = true;
  std::uint64_t chaos_end_ns = 0;
  std::uint64_t stalled_until = 0;
  std::uint64_t pub_blocked_until = 0;
  std::deque<std::pair<int, Buf>> stalled_requests;
  bool sabotage_done = false;
  Result res;
  std::uint64_t hash = 0xCBF29CE484222325ull;

  World(const Config& c, Sabotage s, const TraceFn* t) : cfg(c), sabotage(s), trace(t), rng(c.seed) {
    out.w = this;
  }

  // ---- helpers -------------------------------------------------------------
  void schedule(std::uint64_t t, Kind k, int sub = -1, std::uint64_t aux = 0, Buf data = nullptr) {
    q.push(Event{t, order++, k, sub, aux, std::move(data)});
  }
  void fail(const std::string& why) {
    if (!res.ok) return;
    res.ok = false;
    res.failure = why;
    res.failure_ns = t_now;
  }
  void mix(std::uint64_t v) noexcept {
    hash ^= v;
    hash *= 0x100000001B3ull;
  }
  int era_of(const protocol::Session& s) const noexcept {
    for (std::size_t i = 0; i < eras.size(); ++i) {
      if (eras[i]->session == s) return static_cast<int>(i);
    }
    return -1;
  }
  bool lose(LinkState& ls, const LinkFaults& f) {
    if (!chaos) return false;  // fairness: after the chaos phase nothing is lost
    if (ls.burst_left > 0) {
      --ls.burst_left;
      return true;
    }
    if (f.burst > 0.0 && rng.chance(f.burst)) {
      ls.burst_left = f.burst_len > 0 ? f.burst_len - 1 : 0;
      return true;
    }
    return f.drop > 0.0 && rng.chance(f.drop);
  }
  std::uint64_t delay(const LinkFaults& f) {
    std::uint64_t d = f.latency_ns;
    if (f.jitter_ns > 0) d += rng.below(f.jitter_ns);
    if (chaos && f.spike > 0.0 && rng.chance(f.spike)) d += rng.below(f.spike_ns + 1);
    return d;
  }
  void transmit(Kind k, int sub, Buf b, LinkState& ls, const LinkFaults& f) {
    ++res.totals.datagrams;
    if (lose(ls, f)) {
      ++res.totals.lost;
      return;
    }
    schedule(t_now + delay(f), k, sub, 0, b);
    if (chaos && f.dup > 0.0 && rng.chance(f.dup)) {
      ++res.totals.duplicated;
      schedule(t_now + delay(f), k, sub, 0, std::move(b));
    }
  }

  // ---- publisher -------------------------------------------------------------
  void start_era() {
    auto e = std::make_unique<Era>();
    char id[16];
    std::snprintf(id, sizeof id, "DST%07u", static_cast<unsigned>(eras.size() + 1));
    e->session = protocol::make_session(id);
    e->log = make_message_log();
    RecoveryConfig rc;
    rc.session = e->session;
    rc.ring_capacity = cfg.ring;
    rc.max_reply_messages = cfg.max_reply;
    e->rec = std::make_unique<RecoveryState>(rc);
    PublisherConfig pc;
    pc.session = e->session;
    pc.max_batch_delay_ns = cfg.batch_delay_ns;
    pc.heartbeat_interval_ns = cfg.heartbeat_ns;
    pc.stamp_publish_ts = false;  // the bytes we keep are the bytes sent
    e->pub = std::make_unique<Publisher<SimOut>>(pc, out, e->log.get(), &sim_clock);
    eras.push_back(std::move(e));
    cur = static_cast<int>(eras.size()) - 1;
    up = true;
    stalled_until = 0;
    pub_blocked_until = 0;
    stalled_requests.clear();
    ++res.totals.sessions;
    const auto era = static_cast<std::uint64_t>(cur);
    if (publishing) {
      schedule(t_now + 1 + rng.below(publish_gap_ns()), Kind::kPublish, -1, era);
    } else {
      eras[static_cast<std::size_t>(cur)]->pub->end_session(3);
    }
    schedule(t_now + cfg.poll_ns, Kind::kPubPoll, -1, era);
    schedule(t_now + 1 + rng.below(cfg.drain_interval_ns), Kind::kRecovery, -1, era);
  }
  std::uint64_t publish_gap_ns() const {
    // Mean gap between publish events, each emitting (1 + max_burst) / 2 messages.
    const double per_event = (1.0 + cfg.max_burst) / 2.0;
    const double gap = 1e6 * per_event / std::max(cfg.msgs_per_ms, 1e-3);
    return std::max<std::uint64_t>(2, static_cast<std::uint64_t>(2.0 * gap));
  }
  Era& era() { return *eras[static_cast<std::size_t>(cur)]; }
  bool current(std::uint64_t e) const { return up && static_cast<int>(e) == cur; }

  bool log_full() { return era().log->size_approx() + 8 >= MessageLog::capacity(); }

  void publish_one() {
    Era& e = era();
    const auto icao = static_cast<std::uint32_t>(1 + rng.below(cfg.icao_pool));
    std::uint8_t buf[protocol::TrackUpdate::kWireSize];
    std::size_t len = 0;
    std::uint64_t old = 0;
    if (auto it = e.track_fp.find(icao); it != e.track_fp.end()) old = it->second;
    if (rng.chance(cfg.delete_share)) {
      protocol::TrackDelete d;
      d.icao = icao;
      d.reason = static_cast<std::uint8_t>(1 + rng.below(2));
      d.event_ts_ns = t_now;
      protocol::write(d, buf);
      len = protocol::TrackDelete::kWireSize;
      e.fp -= old;
      e.track_fp.erase(icao);
    } else {
      protocol::TrackUpdate u;
      u.icao = icao;
      u.update_mask = static_cast<std::uint16_t>(rng.next());
      u.event_ts_ns = t_now;
      u.radio_ts = rng.next();
      // In 64 bits: 3.6e9 does not fit an int32 before the subtraction.
      u.lat_e7 = static_cast<std::int32_t>(static_cast<std::int64_t>(rng.below(1'800'000'000)) - 900'000'000);
      u.lon_e7 = static_cast<std::int32_t>(static_cast<std::int64_t>(rng.below(3'600'000'000)) - 1'800'000'000);
      u.alt_ft = static_cast<std::int32_t>(rng.below(45'000));
      u.gs_dkt = static_cast<std::uint16_t>(rng.below(6000));
      u.squawk = static_cast<std::uint16_t>(rng.below(0x7778));
      for (char& c : u.callsign) c = static_cast<char>('A' + rng.below(26));
      u.publish_ts_ns = t_now;
      protocol::write(u, buf);
      len = protocol::TrackUpdate::kWireSize;
      const std::uint64_t h = FeedBook::track_fingerprint(u);
      e.fp += h - old;
      e.track_fp[icao] = h;
    }
    const std::span<const std::uint8_t> m(buf, len);
    e.truth.apply(m);
    e.bytes.insert(e.bytes.end(), m.begin(), m.end());
    e.off.push_back(static_cast<std::uint32_t>(e.bytes.size()));
    e.fp_at.push_back(e.fp);
    ++res.totals.published;
    e.pub->publish(buf, static_cast<std::uint16_t>(len));
  }

  // ---- recovery --------------------------------------------------------------
  void serve_request(int sub, const Buf& req) {
    Era& e = era();
    (void)e.rec->drain(*e.log);
    Sub& s = *subs[static_cast<std::size_t>(sub)];
    e.rec->handle_request(*req, [&](std::span<const std::uint8_t> d) {
      auto b = std::make_shared<Bytes>(d.begin(), d.end());
      if (sabotage == Sabotage::kCorruptReply && !sabotage_done && b->size() > 20 + 2 + 8) {
        (*b)[20 + 2 + 7] ^= 0x01;  // inside the first message (header 20, length 2): its icao
        sabotage_done = true;
      }
      transmit(Kind::kReply, sub, std::move(b), s.rep_ls, s.spec.reply);
    });
  }

  // ---- subscribers -----------------------------------------------------------
  int sub_era(const Sub& s) const { return s.core->has_session() ? era_of(s.core->session()) : -1; }
  // Records a change of session; `by_datagram`: caused by a feed or reply
  // datagram (not by the snapshot server's answer).
  void session_moved(Sub& s, int before, bool by_datagram) {
    const int now_era = sub_era(s);
    if (now_era == before || now_era < 0) return;
    // Only recently left sessions: SubscriberCore remembers 8 retired ids,
    // and each switch to a stray session and back can cost two of them.
    const auto from = s.left.size() > 4 ? s.left.end() - 4 : s.left.begin();
    if (by_datagram && std::find(from, s.left.end(), now_era) != s.left.end()) {
      // Only the server may send a subscriber back to a session it left
      // (undoing a switch to a stray one); a late datagram of it is stale.
      char msg[160];
      std::snprintf(msg, sizeof msg, "subscriber %d went back to session %d, which it had left, on a stale datagram",
                    s.idx, now_era + 1);
      fail(msg);
    }
    if (before >= 0 && std::find(s.left.begin(), s.left.end(), before) == s.left.end()) s.left.push_back(before);
  }
  void deliver(Sub& s, Source src, const Buf& b) {
    const int before = sub_era(s);
    s.core->on_datagram(*b, src, t_now);
    session_moved(s, before, true);
    check(s);
  }
  void after_io(Sub& s) {
    s.core->poll(t_now);
    check(s);
    if (!s.fetching && s.core->take_snapshot_request(t_now)) start_fetch(s);
  }
  void start_fetch(Sub& s) {
    s.fetching = true;
    ++s.fetch_id;
    s.fetched.reset();
    s.fetched_era = -1;
    if (!up || (chaos && cfg.snap_fail > 0.0 && rng.chance(cfg.snap_fail))) {
      // Connection refused (no publisher) or failed.
      schedule(t_now + cfg.snap_connect_ns, Kind::kSnapDone, s.idx, s.fetch_id);
      return;
    }
    schedule(t_now + cfg.snap_connect_ns / 2 + 1, Kind::kSnapServe, s.idx, s.fetch_id);
  }
  void snap_serve(Sub& s, std::uint64_t id) {
    if (id != s.fetch_id) return;
    if (!up) {
      schedule(t_now + cfg.snap_connect_ns / 2 + 1, Kind::kSnapDone, s.idx, id);
      return;
    }
    if (t_now < stalled_until) {  // the server accepts when its thread runs again
      schedule(stalled_until, Kind::kSnapServe, s.idx, id);
      return;
    }
    Era& e = era();
    (void)e.rec->drain(*e.log);
    Bytes bytes;
    e.rec->make_snapshot(bytes);
    SnapshotParser parser;
    if (parser.feed(bytes) != SnapshotParser::State::kDone) {
      fail(std::string("the snapshot server produced an invalid stream: ") + parser.error());
      return;
    }
    s.fetched = parser.data();
    if (sabotage == Sabotage::kSkipSnapshotSeq && !sabotage_done && s.fetched->as_of_seq > 0) {
      ++s.fetched->as_of_seq;
      sabotage_done = true;
    }
    s.fetched_era = cur;
    s.flushed_if_dead = rng.chance(0.5);
    if (chaos && cfg.snap_cut > 0.0 && rng.chance(cfg.snap_cut)) s.fetched.reset();  // stream cut
    const std::uint64_t transfer =
        cfg.snap_connect_ns / 2 + 1 + cfg.snap_ns_per_track * static_cast<std::uint64_t>(e.rec->book().size());
    schedule(t_now + transfer, Kind::kSnapDone, s.idx, id);
  }
  void snap_done(Sub& s, std::uint64_t id) {
    if (id != s.fetch_id || !s.fetching) return;
    // A server process that dies during the transfer either resets the
    // connection or (its kernel flushing the socket) still delivers the
    // whole stream: a complete snapshot of a session that is gone.
    const bool server_alive = s.fetched_era == cur && up;
    const bool ok = s.fetched.has_value() && (server_alive || s.flushed_if_dead);
    if (ok) {
      const int before = sub_era(s);
      s.core->on_snapshot(*s.fetched, t_now);
      session_moved(s, before, false);
    } else {
      s.core->on_snapshot_failed(t_now);
    }
    s.fetched.reset();
    s.fetching = false;
    check(s);
    // Blocking client: what queued in the kernel meanwhile, in arrival order.
    while (res.ok && !s.queue.empty()) {
      auto [src, b] = std::move(s.queue.front());
      s.queue.pop_front();
      deliver(s, src, b);
    }
    if (res.ok) after_io(s);
  }
  void arrive(Sub& s, Source src, Buf b) {
    if (s.fetching && s.spec.blocking_snapshot) {
      if (s.queue.size() >= s.spec.queue_limit) {
        ++res.totals.queue_overflow;
        return;
      }
      s.queue.emplace_back(src, std::move(b));
      return;
    }
    deliver(s, src, b);
    if (res.ok) after_io(s);
  }

  // ---- invariants ------------------------------------------------------------
  void check(Sub& s) {
    ++res.totals.checks;
    trace_sub(s);
    const SubscriberCore& c = *s.core;
    if (!c.has_session()) return;
    const int e = era_of(c.session());
    char msg[256];
    if (e < 0) {
      std::snprintf(msg, sizeof msg, "subscriber %d adopted a session that was never published", s.idx);
      fail(msg);
      return;
    }
    // A subscriber may step into an older session it never saw (a late
    // packet around a restart: ids are opaque), but only unconfirmed
    // (confirming(), or in snapshot mode) until the server's snapshot brings
    // it back. It must never be live, confirmed, in a session older than one
    // it was live in.
    if (c.mode() != SubscriberCore::Mode::kLive) return;
    if (e < s.max_era && !c.confirming()) {
      std::snprintf(msg, sizeof msg, "subscriber %d is live in session %d after being live in session %d", s.idx,
                    e + 1, s.max_era + 1);
      fail(msg);
      return;
    }
    s.max_era = std::max(s.max_era, e);
    const std::uint64_t n = c.expected() - 1;
    if (e == s.chk_era && n == s.chk_n && !s.dirty) return;
    s.chk_era = e;
    s.chk_n = n;
    s.dirty = false;
    const Era& era_ref = *eras[static_cast<std::size_t>(e)];
    if (n > era_ref.count()) {
      std::snprintf(msg, sizeof msg,
                    "subscriber %d is live at message %" PRIu64 " of session %d, which published only %" PRIu64,
                    s.idx, n, e + 1, era_ref.count());
      fail(msg);
      return;
    }
    if (c.book().fingerprint() != era_ref.fp_at[n]) {
      std::snprintf(msg, sizeof msg,
                    "subscriber %d's book differs from the publisher's after message %" PRIu64
                    " of session %d (%zu tracks)",
                    s.idx, n, e + 1, c.book().size());
      fail(msg);
    }
  }
  void on_applied(Sub& s, std::uint64_t seq, std::span<const std::uint8_t> m) {
    char msg[256];
    const int e = era_of(s.core->session());
    if (e < 0) return;  // reported by check()
    const Era& era_ref = *eras[static_cast<std::size_t>(e)];
    if (!s.fresh && seq != s.last_seq + 1) {
      std::snprintf(msg, sizeof msg, "subscriber %d applied message %" PRIu64 " after %" PRIu64 " (session %d)",
                    s.idx, seq, s.last_seq, e + 1);
      fail(msg);
    } else if (seq == 0 || seq > era_ref.count()) {
      std::snprintf(msg, sizeof msg, "subscriber %d applied message %" PRIu64 " of session %d, never published",
                    s.idx, seq, e + 1);
      fail(msg);
    } else {
      const auto want = era_ref.msg(seq);
      if (want.size() != m.size() || std::memcmp(want.data(), m.data(), m.size()) != 0) {
        std::snprintf(msg, sizeof msg,
                      "subscriber %d applied message %" PRIu64 " of session %d with different bytes than published",
                      s.idx, seq, e + 1);
        fail(msg);
      }
    }
    s.fresh = false;
    s.last_seq = seq;
  }

  bool converged() const {
    // The final publisher must be running: a chaos phase that ended during
    // a restart's downtime is not over until the new session has started.
    if (!up || cur != static_cast<int>(eras.size()) - 1) return false;
    const Era& last = *eras.back();
    for (const auto& s : subs) {
      const SubscriberCore& c = *s->core;
      if (!s->joined || s->fetching || !c.has_session() || c.session() != last.session || !c.ended()) return false;
      if (c.book().fingerprint() != last.fp) return false;
    }
    return true;
  }

  // ---- tracing ---------------------------------------------------------------
  void trace_event(const Event& ev, const char* when) {
    char line[320];
    int n = std::snprintf(line, sizeof line, "%s%-10s", when, kind_name(ev.kind));
    if (ev.sub >= 0) n += std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), " sub %d", ev.sub);
    if (ev.kind == Kind::kPublish || ev.kind == Kind::kPubPoll || ev.kind == Kind::kRecovery ||
        ev.kind == Kind::kRestart) {
      n += std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), " era %" PRIu64, ev.aux + 1);
    }
    if (ev.data && ev.data->size() >= protocol::PacketHeader::kWireSize) {
      protocol::PacketHeader h;
      protocol::read(ev.data->data(), h);
      const int e = era_of(h.session);
      const char* what = h.count == protocol::kEndOfSessionCount ? " end-of-session"
                         : h.count == protocol::kHeartbeatCount  ? " heartbeat"
                                                                 : "";
      n += std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n),
                         " | session %d seq %" PRIu64 " count %u%s", e + 1, h.seq, h.count, what);
    }
    (*trace)(t_now, line);
  }
  void trace_sub(const Sub& s) {
    if (trace == nullptr) return;
    const SubscriberCore& c = *s.core;
    char line[256];
    std::snprintf(line, sizeof line, "  -> sub %d: session %d, %s, expected %" PRIu64 ", known %" PRIu64 "%s%s",
                  s.idx, c.has_session() ? era_of(c.session()) + 1 : 0,
                  c.mode() == SubscriberCore::Mode::kLive ? "live" : "snapshot mode", c.expected(), c.known_next(),
                  c.ended() ? ", ended" : "", s.fetching ? ", fetching" : "");
    (*trace)(t_now, line);
  }

  // ---- the run ---------------------------------------------------------------
  void step(const Event& ev) {
    t_now = ev.t;
    ++res.events;
    mix(ev.t);
    mix(static_cast<std::uint64_t>(ev.kind) << 32 | static_cast<std::uint32_t>(ev.sub + 1));
    mix(ev.aux);
    if (ev.data) {
      mix(ev.data->size());
      std::uint64_t head = 0;
      std::memcpy(&head, ev.data->data(), std::min<std::size_t>(8, ev.data->size()));
      mix(head);
      if (ev.data->size() >= 20) {
        std::uint64_t seq = 0;
        std::memcpy(&seq, ev.data->data() + 10, 8);
        mix(seq);
      }
    }
    if (trace != nullptr) trace_event(ev, "");
    switch (ev.kind) {
      case Kind::kPublish: {
        if (!current(ev.aux) || !publishing) break;
        const auto n = 1 + rng.below(cfg.max_burst);
        bool blocked = false;
        for (std::uint64_t i = 0; i < n && !blocked; ++i) {
          if (log_full()) {
            if (t_now < stalled_until) {
              // Publisher::log_message spins until the recovery thread drains:
              // the publisher (feed, batches, heartbeats) stops until then.
              ++res.totals.publisher_blocked;
              pub_blocked_until = stalled_until;
              schedule(stalled_until, Kind::kPublish, -1, ev.aux);
              blocked = true;
              break;
            }
            // Not stalled: the recovery thread would have drained by now.
            ++res.totals.forced_drains;
            (void)era().rec->drain(*era().log);
          }
          publish_one();
        }
        if (!blocked) schedule(t_now + 1 + rng.below(publish_gap_ns()), Kind::kPublish, -1, ev.aux);
        break;
      }
      case Kind::kPubPoll:
        if (!current(ev.aux)) break;
        if (t_now < pub_blocked_until) {  // spinning on the full log
          schedule(pub_blocked_until, Kind::kPubPoll, -1, ev.aux);
          break;
        }
        era().pub->poll();
        schedule(t_now + cfg.poll_ns, Kind::kPubPoll, -1, ev.aux);
        break;
      case Kind::kRecovery: {
        if (!current(ev.aux)) break;
        Era& e = era();
        (void)e.rec->drain(*e.log);
        while (!stalled_requests.empty()) {
          auto [sub, b] = std::move(stalled_requests.front());
          stalled_requests.pop_front();
          serve_request(sub, b);
        }
        std::uint64_t next = t_now + 1 + rng.below(cfg.drain_interval_ns);
        if (chaos && cfg.stall_chance > 0.0 && rng.chance(cfg.stall_chance)) {
          ++res.totals.stalls;
          stalled_until = t_now + 1 + rng.below(cfg.stall_ns + 1);
          next = stalled_until;
        }
        schedule(next, Kind::kRecovery, -1, ev.aux);
        break;
      }
      case Kind::kRequest:
        if (!up) break;  // nobody listening: lost
        if (t_now < stalled_until) {
          stalled_requests.emplace_back(ev.sub, ev.data);
        } else {
          serve_request(ev.sub, ev.data);
        }
        break;
      case Kind::kFeed:
        arrive(*subs[static_cast<std::size_t>(ev.sub)], Source::kFeed, ev.data);
        break;
      case Kind::kReply:
        arrive(*subs[static_cast<std::size_t>(ev.sub)], Source::kRetransmit, ev.data);
        break;
      case Kind::kSubTick: {
        Sub& s = *subs[static_cast<std::size_t>(ev.sub)];
        if (!(s.fetching && s.spec.blocking_snapshot)) after_io(s);
        schedule(t_now + 1'000'000, Kind::kSubTick, ev.sub);
        break;
      }
      case Kind::kSnapServe:
        snap_serve(*subs[static_cast<std::size_t>(ev.sub)], ev.aux);
        break;
      case Kind::kSnapDone:
        snap_done(*subs[static_cast<std::size_t>(ev.sub)], ev.aux);
        break;
      case Kind::kRestart: {
        if (!up) break;
        const Config::Restart& r = cfg.restarts[ev.aux];
        Era& e = era();
        if (!r.crash) e.pub->end_session(3);
        e.alive = false;
        up = false;
        stalled_requests.clear();
        schedule(t_now + r.downtime_ns, Kind::kUp);
        break;
      }
      case Kind::kUp:
        start_era();
        break;
      case Kind::kJoin: {
        Sub& s = *subs[static_cast<std::size_t>(ev.sub)];
        s.joined = true;
        schedule(t_now + 1 + rng.below(1'000'000), Kind::kSubTick, ev.sub);
        break;
      }
      case Kind::kChaosEnd:
        chaos = false;
        publishing = false;
        if (up) era().pub->end_session(3);
        break;
    }
  }

  Result go() {
    t_now = 1;
    subs.reserve(cfg.subs.size());
    for (std::size_t i = 0; i < cfg.subs.size(); ++i) {
      auto s = std::make_unique<Sub>();
      s->w = this;
      s->idx = static_cast<int>(i);
      s->spec = cfg.subs[i];
      s->core = std::make_unique<SubscriberCore>(s->spec.core, s.get(), s.get());
      subs.push_back(std::move(s));
      schedule(std::max<std::uint64_t>(1, cfg.subs[i].join_ns), Kind::kJoin, static_cast<int>(i));
    }
    for (std::size_t i = 0; i < cfg.restarts.size(); ++i) {
      if (cfg.restarts[i].at_ns < cfg.chaos_ns) schedule(cfg.restarts[i].at_ns, Kind::kRestart, -1, i);
    }
    chaos_end_ns = cfg.chaos_ns;
    schedule(chaos_end_ns, Kind::kChaosEnd);
    start_era();
    std::uint64_t deadline = chaos_end_ns + cfg.settle_limit_ns;
    const std::uint64_t cap = chaos_end_ns + std::max(cfg.settle_limit_ns, cfg.settle_cap_ns);
    std::vector<std::uint64_t> expected_at(subs.size(), 0);
    bool extended = false;
    while (res.ok && !q.empty()) {
      const Event& top = q.top();
      if (top.t > deadline) {
        // Past the limit: more time only while every unfinished subscriber
        // keeps advancing (see Config::settle_cap_ns). The first extension
        // has no earlier reading to compare with, so a stuck subscriber
        // fails one window after the limit.
        bool advancing = deadline + cfg.progress_window_ns <= cap;
        for (std::size_t i = 0; i < subs.size() && advancing; ++i) {
          const SubscriberCore& c = *subs[i]->core;
          if (!c.ended() && extended && c.expected() <= expected_at[i]) advancing = false;
        }
        if (!advancing) {
          t_now = deadline;
          break;
        }
        for (std::size_t i = 0; i < subs.size(); ++i) expected_at[i] = subs[i]->core->expected();
        extended = true;
        deadline += cfg.progress_window_ns;
        continue;
      }
      Event ev = top;
      q.pop();
      step(ev);
      if (res.events > cfg.max_events) {
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "runaway: more than %" PRIu64 " events (a request or retransmit storm?)", cfg.max_events);
        fail(msg);
        break;
      }
      if (!chaos && res.ok && converged()) break;
    }
    res.sim_ns = t_now;
    res.trace_hash = hash;
    if (res.ok) finish();
    for (const auto& s : subs) {
      const SubscriberStats& st = s->core->stats();
      res.totals.applied += st.applied;
      res.totals.gaps += st.gaps;
      res.totals.requests += st.requests;
      res.totals.request_retries += st.request_retries;
      res.totals.naks += st.naks;
      res.totals.snapshots_loaded += st.snapshots_loaded;
      res.totals.snapshot_failures += st.snapshot_failures;
      res.totals.session_changes += st.session_changes;
      res.totals.stale_packets += st.stale_packets;
      res.totals.window_overflow += st.window_overflow;
      res.totals.truncated_replies += st.truncated_replies;
      res.totals.duplicates += st.duplicates;
    }
    return res;
  }

  void finish() {
    char msg[512];
    if (!converged()) {
      std::string why = "liveness: not every subscriber reached the end of the final session within the limit (or stopped advancing after it):";
      for (const auto& s : subs) {
        const SubscriberCore& c = *s->core;
        std::snprintf(msg, sizeof msg,
                      " [sub %d: joined %d, session %d, mode %s, expected %" PRIu64 ", known %" PRIu64
                      ", ended %d, fetching %d, request outstanding %d]",
                      s->idx, s->joined ? 1 : 0, c.has_session() ? era_of(c.session()) + 1 : 0,
                      c.mode() == SubscriberCore::Mode::kLive ? "live" : "snapshot", c.expected(), c.known_next(),
                      c.ended() ? 1 : 0, s->fetching ? 1 : 0, c.request_outstanding() ? 1 : 0);
        why += msg;
      }
      std::snprintf(msg, sizeof msg, " (final session %zu published %" PRIu64 ")", eras.size(),
                    eras.back()->count());
      why += msg;
      fail(why);
      return;
    }
    const Era& last = *eras.back();
    if (last.truth.fingerprint() != last.fp) {
      fail("simulator bug: the incremental fingerprint differs from the truth book's");
      return;
    }
    for (const auto& s : subs) {
      if (s->core->book().digest() != last.truth.digest()) {
        std::snprintf(msg, sizeof msg, "subscriber %d ended with a book whose SHA-256 differs from the publisher's",
                      s->idx);
        fail(msg);
        return;
      }
    }
    for (const auto& e : eras) {
      if (e->rec->stats().log_errors != 0) {
        fail("the recovery side saw out-of-sequence log entries");
        return;
      }
    }
    if (up) {
      Era& e = era();
      (void)e.rec->drain(*e.log);
      if (!(e.rec->book() == e.truth)) fail("the snapshot server's book differs from the publisher's");
    }
  }
};

bool SimOut::send(std::span<const std::uint8_t> d) noexcept {
  auto b = std::make_shared<const Bytes>(d.begin(), d.end());
  for (auto& s : w->subs) {
    if (s->joined) w->transmit(Kind::kFeed, s->idx, b, s->feed_ls, s->spec.feed);
  }
  return true;
}

void Sub::send_request(const protocol::RetransmitRequest& req) {
  auto b = std::make_shared<Bytes>(protocol::RetransmitRequest::kWireSize);
  protocol::write(req, b->data());
  w->transmit(Kind::kRequest, idx, std::move(b), req_ls, spec.request);
}

void Sub::on_message(std::uint64_t seq, std::span<const std::uint8_t> msg) { w->on_applied(*this, seq, msg); }

// ---- random worlds ------------------------------------------------------------

std::uint64_t pick(Rng& r, std::initializer_list<std::uint64_t> v) {
  return *(v.begin() + static_cast<std::ptrdiff_t>(r.below(v.size())));
}
std::uint64_t range(Rng& r, std::uint64_t lo, std::uint64_t hi) { return lo + r.below(hi - lo + 1); }
// Roughly log-uniform in [lo, hi] (lo >= 1): a power-of-two octave first,
// then a value inside it, so small and large values are about equally
// likely per octave. Integer arithmetic only: libm functions such as pow may
// round differently across platforms, and a seed must build the same world
// everywhere.
std::uint64_t log_range(Rng& r, std::uint64_t lo, std::uint64_t hi) {
  auto bits = [](std::uint64_t v) {
    int b = 0;
    while (v >>= 1) ++b;
    return b;
  };
  const int b = bits(lo) + static_cast<int>(r.below(static_cast<std::uint64_t>(bits(hi) - bits(lo) + 1)));
  const std::uint64_t base = std::uint64_t{1} << b;
  return std::clamp(base + r.below(base), lo, hi);
}

LinkFaults random_link(Rng& r, bool feed) {
  LinkFaults f;
  if (!r.chance(0.2)) f.drop = r.uniform(0.0, feed ? 0.3 : 0.2);
  if (r.chance(0.4)) {
    f.burst = r.uniform(0.0, 0.01);
    f.burst_len = static_cast<std::uint32_t>(log_range(r, 2, feed ? 3000 : 50));
  }
  if (r.chance(0.5)) f.dup = r.uniform(0.0, 0.1);
  f.latency_ns = log_range(r, 5'000, 2'000'000);
  if (r.chance(0.7)) f.jitter_ns = log_range(r, 1'000, 3'000'000);
  if (r.chance(0.3)) {
    f.spike = r.uniform(0.0, 0.02);
    f.spike_ns = log_range(r, 1'000'000, 200'000'000);
  }
  return f;
}

}  // namespace

Config random_config(std::uint64_t seed, Profile profile) {
  // A separate stream from the run's (seeded differently), so changing how a
  // world is drawn does not change what the run does with it... and vice versa.
  Rng r(seed ^ 0xD57C0FF16E5EEDull);
  const bool deep = profile == Profile::kDeep;
  Config c;
  c.seed = seed;
  c.max_events = deep ? 50'000'000 : 5'000'000;  // normal: about 4k (quick), up to 5M (deep)
  c.chaos_ns = deep ? log_range(r, 100'000'000, 3'000'000'000) : log_range(r, 10'000'000, 150'000'000);
  c.msgs_per_ms = static_cast<double>(log_range(r, 100, deep ? 100'000 : 20'000)) / 1000.0;
  c.max_burst = static_cast<std::uint32_t>(pick(r, {1, 1, 4, 32}));
  c.icao_pool = static_cast<std::uint32_t>(pick(r, {1, 3, 20, 100, 400}));
  c.delete_share = r.chance(0.2) ? 0.0 : r.uniform(0.0, 0.4);
  c.batch_delay_ns = r.chance(0.5) ? 0 : log_range(r, 1'000, 3'000'000);
  c.heartbeat_ns = log_range(r, 500'000, 50'000'000);
  c.poll_ns = log_range(r, 20'000, 500'000);
  const std::uint64_t nrestart = r.chance(0.5) ? 0 : range(r, 1, deep ? 5 : 3);
  for (std::uint64_t i = 0; i < nrestart; ++i) {
    Config::Restart rs;
    rs.at_ns = r.below(c.chaos_ns);
    rs.crash = r.chance(0.5);
    rs.downtime_ns = r.chance(0.3) ? 0 : log_range(r, 10'000, 50'000'000);
    c.restarts.push_back(rs);
  }
  std::sort(c.restarts.begin(), c.restarts.end(),
            [](const Config::Restart& a, const Config::Restart& b) { return a.at_ns < b.at_ns; });
  c.ring = pick(r, {16, 64, 256, 4096, 65536});
  // Below max_request (256, 1024) so truncated answers are common; not
  // smaller: a cap of a few messages makes recovery a matter of throughput
  // (cap per round trip), which no liveness bound can sensibly cover.
  c.max_reply = static_cast<std::uint32_t>(pick(r, {64, 1024, 4096}));
  c.drain_interval_ns = log_range(r, 20'000, 1'000'000);
  if (r.chance(0.4)) {
    c.stall_chance = r.uniform(0.0, 0.05);
    c.stall_ns = log_range(r, 1'000'000, 50'000'000);
  }
  c.snap_connect_ns = log_range(r, 20'000, 5'000'000);
  c.snap_ns_per_track = log_range(r, 10, 5'000);
  if (r.chance(0.5)) c.snap_fail = r.uniform(0.0, 0.4);
  if (r.chance(0.3)) c.snap_cut = r.uniform(0.0, 0.3);
  const std::uint64_t nsub = range(r, 1, deep ? 6 : 4);
  for (std::uint64_t i = 0; i < nsub; ++i) {
    SubscriberSpec s;
    s.join_ns = r.chance(0.5) ? 0 : r.below(c.chaos_ns);
    s.blocking_snapshot = r.chance(0.5);
    s.queue_limit = static_cast<std::uint32_t>(log_range(r, 8, 8192));
    s.core.window = static_cast<std::uint32_t>(pick(r, {8, 64, 1024, 16384}));
    // max_request 1 recovers one message per round trip: slow by
    // configuration, not a protocol failure, and no real setting.
    s.core.max_request = static_cast<std::uint16_t>(pick(r, {16, 256, 1024}));
    s.core.retransmit_timeout_ns = log_range(r, 100'000, 30'000'000);
    s.core.snapshot_retry_ns = log_range(r, 1'000'000, 200'000'000);
    s.feed = random_link(r, true);
    s.request = random_link(r, false);
    s.reply = random_link(r, false);
    c.subs.push_back(s);
  }
  return c;
}

std::string describe(const Config& c) {
  std::string out;
  char b[512];
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::snprintf(b, sizeof b,
                "seed %llu: chaos %.1f ms, %.2f msgs/ms (bursts of 1..%u), %u aircraft, %.0f%% deletes\n"
                "  publisher: batch %llu us, heartbeat %.1f ms, poll %llu us, %zu restart(s)\n",
                u(c.seed), static_cast<double>(c.chaos_ns) / 1e6, c.msgs_per_ms, c.max_burst, c.icao_pool,
                c.delete_share * 100, u(c.batch_delay_ns / 1000), static_cast<double>(c.heartbeat_ns) / 1e6,
                u(c.poll_ns / 1000), c.restarts.size());
  out += b;
  for (const auto& r : c.restarts) {
    std::snprintf(b, sizeof b, "    at %.2f ms: %s, down %.2f ms\n", static_cast<double>(r.at_ns) / 1e6,
                  r.crash ? "crash" : "clean end", static_cast<double>(r.downtime_ns) / 1e6);
    out += b;
  }
  std::snprintf(b, sizeof b,
                "  recovery: ring %zu, max reply %u, drain every <= %llu us, stalls %.3f x <= %.1f ms\n"
                "  snapshots: connect %llu us, %llu ns/track, fail %.2f, cut %.2f\n",
                c.ring, c.max_reply, u(c.drain_interval_ns / 1000), c.stall_chance,
                static_cast<double>(c.stall_ns) / 1e6, u(c.snap_connect_ns / 1000), u(c.snap_ns_per_track),
                c.snap_fail, c.snap_cut);
  out += b;
  auto link = [&](const char* name, const LinkFaults& f) {
    std::snprintf(b, sizeof b,
                  "      %-7s drop %.3f, bursts %.4f x %u, dup %.3f, latency %llu us + jitter %llu us, "
                  "spikes %.3f x <= %.1f ms\n",
                  name, f.drop, f.burst, f.burst_len, f.dup, u(f.latency_ns / 1000), u(f.jitter_ns / 1000), f.spike,
                  static_cast<double>(f.spike_ns) / 1e6);
    out += b;
  };
  for (std::size_t i = 0; i < c.subs.size(); ++i) {
    const SubscriberSpec& s = c.subs[i];
    std::snprintf(b, sizeof b,
                  "  subscriber %zu: joins at %.2f ms, %s snapshots (queue %u), window %u, max request %u, "
                  "retransmit timeout %.2f ms, snapshot retry %.1f ms\n",
                  i, static_cast<double>(s.join_ns) / 1e6, s.blocking_snapshot ? "blocking" : "async",
                  s.queue_limit, s.core.window, s.core.max_request,
                  static_cast<double>(s.core.retransmit_timeout_ns) / 1e6,
                  static_cast<double>(s.core.snapshot_retry_ns) / 1e6);
    out += b;
    link("feed", s.feed);
    link("request", s.request);
    link("reply", s.reply);
  }
  return out;
}

Result run(const Config& cfg, Sabotage sabotage, const TraceFn* trace) {
  World w(cfg, sabotage, trace);
  return w.go();
}

Result run(const Config& cfg, const TraceFn* trace) { return run(cfg, Sabotage::kNone, trace); }

}  // namespace adsb::feed::dst
