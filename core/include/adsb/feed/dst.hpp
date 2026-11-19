#pragma once

// Deterministic simulation testing of the feed (README_UPDATES.md §12, §8 H).
//
// One process runs the real feed code: Publisher, the recovery side
// (RecoveryState: retransmit store, book, snapshots) and several
// SubscriberCores, connected by a simulated network and driven by a
// simulated clock. Nothing else is simulated: the code under test is the
// production code, unchanged.
//
// A discrete-event engine orders everything (datagrams in flight, timers, the
// recovery thread's loop, snapshot fetches, publisher crashes and restarts)
// by simulated time, with ties broken by scheduling order. All randomness
// comes from one Rng seeded by the run's seed, so a run is a pure function of
// its Config: any failure replays exactly from its seed (adsb-dst --seed N).
//
// random_config(seed) draws a different world per seed ("swarm testing"):
// how many subscribers, when they join, whether they fetch snapshots blocking
// (like SubscriberClient: nothing else happens during a fetch, datagrams wait
// in a bounded kernel queue) or asynchronously (like the gateway's JS port),
// per-link loss, loss bursts long enough to be outages, duplication, latency
// jitter (which reorders) and rare long delays, recovery-thread stalls, a
// small or large retransmit ring, snapshot failures and cut streams, and
// publisher crashes or clean restarts with downtime in between.
//
// Checked after every step that touches a subscriber:
//   * every message a subscriber applies is, byte for byte, the message the
//     publisher sent with that sequence number in that session, and they are
//     applied in order with no gap or repeat (between resets and snapshots);
//   * in live mode its book equals the publisher's book after message
//     expected - 1 of the session it is in (TLA+ PrefixOfPublisher; compared
//     with FeedBook::fingerprint());
//   * it is never live in a session older than one it was live in (it may
//     step into an older session it never saw, unconfirmed, until the
//     snapshot server's answer brings it back), and it never goes back to a
//     session it has left because of a datagram (only the server's
//     snapshot may send it back: undoing a switch to a stray session).
// After the chaos phase faults stop (fairness, as in the TLA+ model), the
// publisher ends its session, and every subscriber must reach the end of
// the final session with a book identical (SHA-256 digest) to the
// publisher's, within a time bound (liveness).
//
// Simplified: a blocking subscriber's kernel queue is one queue in arrival
// order with one datagram limit (the real client has two sockets, read 256
// datagrams at a time each, with byte limits); the snapshot server's
// 16-client cap is not modeled; the final publisher never exits. A server
// that dies during a snapshot transfer delivers the whole stream half the
// time (its kernel flushes the socket) and resets it otherwise. A full
// publisher log during a recovery stall blocks the publisher, as in
// Publisher::log_message, but needs more than the simulated rates.
//
// Not modeled: corrupted datagrams (UDP checksums drop them: that is loss),
// the socket and syscall layer itself (tested over real sockets by
// check-feed.sh and the transport jobs).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "adsb/feed/subscriber.hpp"

namespace adsb::feed::dst {

// Faults of one direction of one path (feed to a subscriber, its requests to
// the retransmit server, the replies back).
struct LinkFaults {
  double drop = 0.0;               // independent loss per datagram
  double burst = 0.0;              // chance that a loss burst starts at a datagram
  std::uint32_t burst_len = 1;     // datagrams lost per burst (long bursts are outages)
  double dup = 0.0;                // delivered twice
  std::uint64_t latency_ns = 50'000;  // one-way base delay
  std::uint64_t jitter_ns = 0;     // + uniform [0, jitter): datagrams overtake each other
  double spike = 0.0;              // chance of a long extra delay (far out of order)
  std::uint64_t spike_ns = 0;      // that delay, at most
};

struct SubscriberSpec {
  std::uint64_t join_ns = 0;          // subscribes to the feed at this time
  bool blocking_snapshot = true;      // SubscriberClient-style fetch (else async, like the gateway)
  std::uint32_t queue_limit = 1024;   // datagrams the kernel holds during a blocking fetch
  SubscriberConfig core;
  LinkFaults feed, request, reply;
};

struct Config {
  std::uint64_t seed = 1;

  // Phases. Publishing and faults last chaos_ns; then faults stop, the
  // publisher ends its session, and every subscriber must have converged
  // within settle_limit_ns.
  std::uint64_t chaos_ns = 200'000'000;
  std::uint64_t settle_limit_ns = 20'000'000'000;
  // A run that executes more events than this fails: a request or
  // retransmit storm (simulated time crawls while events pile up).
  std::uint64_t max_events = 50'000'000;

  // Traffic.
  double msgs_per_ms = 5.0;           // mean rate
  std::uint32_t max_burst = 1;        // messages per publish event, 1..max_burst
  std::uint32_t icao_pool = 50;       // distinct aircraft
  double delete_share = 0.1;

  // Publisher.
  std::uint64_t batch_delay_ns = 0;   // 0 = one datagram per message
  std::uint64_t heartbeat_ns = 10'000'000;
  std::uint64_t poll_ns = 100'000;    // how often the publisher's loop polls

  // Restarts during the chaos phase: at each time the publisher crashes (no
  // end-of-session marker) or ends cleanly, is down for downtime_ns, and
  // comes back with a new session.
  struct Restart {
    std::uint64_t at_ns = 0;
    bool crash = false;
    std::uint64_t downtime_ns = 0;
  };
  std::vector<Restart> restarts;

  // Recovery side (its own thread in the real system: a loop that drains
  // the log at least every drain_interval_ns, then serves requests).
  std::size_t ring = 1u << 16;
  std::uint32_t max_reply = 4096;
  std::uint64_t drain_interval_ns = 1'000'000;
  double stall_chance = 0.0;          // per loop pass: the thread stalls (preempted)
  std::uint64_t stall_ns = 0;         // for up to this long

  // Snapshot server.
  std::uint64_t snap_connect_ns = 200'000;       // connect + request
  std::uint64_t snap_ns_per_track = 500;         // transfer time per track
  double snap_fail = 0.0;                        // connection fails
  double snap_cut = 0.0;                         // stream cut before the end

  std::vector<SubscriberSpec> subs;
};

enum class Profile : std::uint8_t {
  kQuick,  // small worlds (tens of ms of traffic): many seeds per second, per-PR CI
  kDeep,   // larger worlds (up to seconds of traffic, more restarts): nightly
};

// The world of `seed` (deterministic).
[[nodiscard]] Config random_config(std::uint64_t seed, Profile profile = Profile::kQuick);
// Multi-line human-readable description (printed with a failure).
[[nodiscard]] std::string describe(const Config& cfg);

// What happened, summed over subscribers (non-vacuity: a sweep should
// exercise every recovery path).
struct Totals {
  std::uint64_t published = 0;
  std::uint64_t sessions = 0;
  std::uint64_t datagrams = 0;
  std::uint64_t lost = 0;               // dropped by the simulated network
  std::uint64_t duplicated = 0;
  std::uint64_t queue_overflow = 0;     // kernel queue full during a blocking fetch
  std::uint64_t stalls = 0;
  std::uint64_t forced_drains = 0;      // log nearly full between two recovery passes
  std::uint64_t publisher_blocked = 0;  // log full during a recovery stall: the publisher waited
  std::uint64_t applied = 0;
  std::uint64_t gaps = 0;
  std::uint64_t requests = 0;
  std::uint64_t request_retries = 0;
  std::uint64_t naks = 0;
  std::uint64_t snapshots_loaded = 0;
  std::uint64_t snapshot_failures = 0;
  std::uint64_t session_changes = 0;
  std::uint64_t stale_packets = 0;
  std::uint64_t window_overflow = 0;
  std::uint64_t truncated_replies = 0;  // capped answers followed up at once
  std::uint64_t duplicates = 0;         // messages a subscriber had already applied
  std::uint64_t checks = 0;             // invariant evaluations

  void add(const Totals& o);
};

struct Result {
  bool ok = true;
  std::string failure;          // the first violated property, with details
  std::uint64_t failure_ns = 0; // simulated time of the violation
  std::uint64_t sim_ns = 0;     // simulated time at the end
  std::uint64_t events = 0;
  std::uint64_t trace_hash = 0; // hash of every event executed: equal runs, equal hashes
  Totals totals;
};

// Called for every event with a one-line description (adsb-dst --trace).
using TraceFn = std::function<void(std::uint64_t now_ns, const std::string& line)>;

[[nodiscard]] Result run(const Config& cfg, const TraceFn* trace = nullptr);

// Self-test hooks: deliberately break something outside the code under test,
// so tests can show that the checks catch it.
enum class Sabotage : std::uint8_t {
  kNone,
  kCorruptReply,     // the network flips a byte inside one retransmitted message
  kSkipSnapshotSeq,  // a snapshot claims to be as of one message later than its content
};
[[nodiscard]] Result run(const Config& cfg, Sabotage sabotage, const TraceFn* trace = nullptr);

}  // namespace adsb::feed::dst
