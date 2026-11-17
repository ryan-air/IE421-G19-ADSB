#pragma once

// Deterministic replay of .adsbrec recordings (README_UPDATES.md §11.2-11.3).
//
//  * replay_inline():   one thread, decode + track state per record. Speed 0
//                       means "as fast as possible"; otherwise records are
//                       released at their original spacing divided by speed.
//  * replay_pipeline(): two pinned threads (reader -> StageQueue -> decode +
//                       state), with per-stage latency histograms. This is the
//                       first slice of the staged pipeline and the harness
//                       for optimized-vs-naive comparisons.
//
// Determinism contract: for one recording and one build, the stream of
// emitted TrackUpdate / TrackDelete messages is byte-identical regardless of
// speed or threading, because the engine only uses event time. HashingSink
// reduces that stream to a SHA-256 so the contract can be checked in CI.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "adsb/common/build_config.hpp"
#include "adsb/common/cache.hpp"
#include "adsb/common/clock.hpp"
#include "adsb/common/hash.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/common/thread.hpp"
#include "adsb/protocol/feed_messages.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/state/track_engine.hpp"

namespace adsb::replay {

// Hashes (and optionally captures) every message the engine emits, in order.
class HashingSink {
 public:
  void on_update(const protocol::TrackUpdate& u) {
    std::uint8_t buf[protocol::TrackUpdate::kWireSize];
    protocol::write(u, buf);
    add(buf, sizeof buf);
    ++updates;
  }
  void on_delete(const protocol::TrackDelete& d) {
    std::uint8_t buf[protocol::TrackDelete::kWireSize];
    protocol::write(d, buf);
    add(buf, sizeof buf);
    ++deletes;
  }
  [[nodiscard]] std::string hex_digest() { return Sha256::hex(sha_.finish()); }

  // If set, every message is appended as {u16 length, bytes}.
  std::vector<std::uint8_t>* capture = nullptr;
  std::uint64_t updates = 0;
  std::uint64_t deletes = 0;

 private:
  void add(const std::uint8_t* p, std::size_t n) {
    sha_.update(p, n);
    if (capture) {
      const auto len = static_cast<std::uint16_t>(n);
      const auto* lp = reinterpret_cast<const std::uint8_t*>(&len);
      capture->insert(capture->end(), lp, lp + 2);
      capture->insert(capture->end(), p, p + n);
    }
  }
  Sha256 sha_;
};

struct ReplayOptions {
  double speed = 0.0;          // 0 = max speed; 1 = original timing; N = N times faster
  bool flush_at_end = false;   // expire every remaining track after the last record
  bool busy_wait = false;      // pace by spinning instead of sleeping (precise; uses a core)
};

struct ReplayResult {
  std::uint64_t records = 0;
  double wall_seconds = 0.0;
  double recording_seconds = 0.0;
  std::string hash;
  std::uint64_t updates = 0;
  std::uint64_t deletes = 0;
  state::EngineStats stats;
};

struct PipelineLatency {
  LatencyHistogram release_lag_ns;  // paced replay: actual - intended release (harness accuracy)
  LatencyHistogram queue_ns;    // enqueue -> dequeue (handoff + backlog)
  LatencyHistogram process_ns;  // dequeue -> decode + state done
  LatencyHistogram total_ns;    // intended release (or enqueue at max speed) -> done
};

namespace detail {

inline state::FrameMeta meta_of(const record::Record& r) {
  state::FrameMeta m;
  m.rx_ns = r.rx_ns;
  m.radio_ts = r.radio_ts;
  m.line = r.line;
  return m;
}

// Waits until mono_ns() >= target: sleeps while far away, then spins for the
// last 2 ms. The spin window is wide because sleeps can overshoot by more than
// a millisecond (timer slack, and especially inside VMs such as Docker on
// macOS); an overshoot would be charged to the pipeline as latency, since
// latency is measured from the intended release time.
inline void wait_until(std::uint64_t target_ns, bool busy_wait = false) {
  constexpr std::uint64_t kSpinWindowNs = 2'000'000;
  for (;;) {
    const std::uint64_t now = mono_ns();
    if (now >= target_ns) return;
    const std::uint64_t left = target_ns - now;
    if (!busy_wait && left > kSpinWindowNs + 500'000) {
      const std::uint64_t sleep_ns = left - kSpinWindowNs;
      timespec ts{static_cast<time_t>(sleep_ns / 1'000'000'000ULL),
                  static_cast<long>(sleep_ns % 1'000'000'000ULL)};
      nanosleep(&ts, nullptr);
    } else {
      cpu_relax();
    }
  }
}

template <typename Engine>
void finish(Engine& eng, HashingSink& sink, std::uint64_t last_rx, const ReplayOptions& opt) {
  if (opt.flush_at_end && last_rx != 0) {
    // Past the end by the track timeout plus two wheel ticks: every track expires.
    eng.advance(last_rx + eng.config().track_timeout_ns + 2'000'000'000ULL, sink);
  }
}

}  // namespace detail

template <typename Engine>
ReplayResult replay_inline(record::RecordingReader& rd, Engine& eng, const ReplayOptions& opt,
                           std::vector<std::uint8_t>* capture = nullptr) {
  HashingSink sink;
  sink.capture = capture;
  ReplayResult res;
  rd.rewind();
  record::Record r;
  std::uint64_t first_rx = 0, last_rx = 0;
  const std::uint64_t wall0 = mono_ns();
  while (rd.next(r)) {
    if (res.records == 0) first_rx = r.rx_ns;
    if (opt.speed > 0.0) {
      const double offset = static_cast<double>(r.rx_ns - first_rx) / opt.speed;
      detail::wait_until(wall0 + static_cast<std::uint64_t>(offset), opt.busy_wait);
    }
    eng.on_frame(r.payload, r.len, detail::meta_of(r), sink);
    last_rx = r.rx_ns;
    ++res.records;
  }
  detail::finish(eng, sink, last_rx, opt);
  res.wall_seconds = static_cast<double>(mono_ns() - wall0) * 1e-9;
  res.recording_seconds = static_cast<double>(last_rx - first_rx) * 1e-9;
  res.hash = sink.hex_digest();
  res.updates = sink.updates;
  res.deletes = sink.deletes;
  res.stats = eng.stats();
  return res;
}

template <typename Engine>
ReplayResult replay_pipeline(record::RecordingReader& rd, Engine& eng, const ReplayOptions& opt,
                             PipelineLatency* lat, int reader_cpu = -1, int worker_cpu = -1,
                             std::vector<std::uint8_t>* capture = nullptr) {
  struct Item {
    record::Record rec;
    std::uint64_t t_release;  // ticks: intended release (paced) or enqueue (max)
    std::uint64_t t_enqueue;  // ticks
  };
  using Queue = StageQueue<Item, 65536>;
  auto queue = std::make_unique<Queue>();
  const TickConverter conv = TickConverter::calibrate();

  std::atomic<bool> reader_done{false};
  std::atomic<std::uint64_t> produced{0};
  ReplayResult res;
  std::uint64_t first_rx = 0, last_rx = 0;
  const std::uint64_t wall0 = mono_ns();
  const std::uint64_t ticks0 = ticks();

  std::thread reader([&] {
    set_current_thread_name("adsb-reader");
    if (reader_cpu >= 0) pin_current_thread(reader_cpu);
    rd.rewind();
    Item it{};
    std::uint64_t n = 0;
    const double ticks_per_ns = conv.ticks_per_second() * 1e-9;
    while (rd.next(it.rec)) {
      if (n == 0) first_rx = it.rec.rx_ns;
      if (opt.speed > 0.0) {
        const double offset_ns = static_cast<double>(it.rec.rx_ns - first_rx) / opt.speed;
        const std::uint64_t target = wall0 + static_cast<std::uint64_t>(offset_ns);
        detail::wait_until(target, opt.busy_wait);
        if (lat) lat->release_lag_ns.record(mono_ns() - target);
        it.t_release = ticks0 + static_cast<std::uint64_t>(offset_ns * ticks_per_ns);
      }
      it.t_enqueue = ticks();
      if (opt.speed <= 0.0) it.t_release = it.t_enqueue;
      while (!queue->try_push(it)) cpu_relax();
      ++n;
      produced.store(n, std::memory_order_release);
    }
    last_rx = it.rec.rx_ns;
    reader_done.store(true, std::memory_order_release);
  });

  HashingSink sink;
  sink.capture = capture;
  std::thread worker([&] {
    set_current_thread_name("adsb-state");
    if (worker_cpu >= 0) pin_current_thread(worker_cpu);
    Item it{};
    std::uint64_t consumed = 0;
    for (;;) {
      if (queue->try_pop(it)) {
        const std::uint64_t t_deq = ticks();
        eng.on_frame(it.rec.payload, it.rec.len, detail::meta_of(it.rec), sink);
        const std::uint64_t t_done = ticks();
        if (lat) {
          lat->queue_ns.record(conv.to_ns(t_deq - it.t_enqueue));
          lat->process_ns.record(conv.to_ns(t_done - t_deq));
          lat->total_ns.record(conv.to_ns(t_done > it.t_release ? t_done - it.t_release : 0));
        }
        ++consumed;
      } else if (reader_done.load(std::memory_order_acquire) &&
                 consumed == produced.load(std::memory_order_acquire)) {
        break;
      } else {
        cpu_relax();
      }
    }
    res.records = consumed;
  });

  reader.join();
  worker.join();
  detail::finish(eng, sink, last_rx, opt);
  res.wall_seconds = static_cast<double>(mono_ns() - wall0) * 1e-9;
  res.recording_seconds = static_cast<double>(last_rx - first_rx) * 1e-9;
  res.hash = sink.hex_digest();
  res.updates = sink.updates;
  res.deletes = sink.deletes;
  res.stats = eng.stats();
  return res;
}

}  // namespace adsb::replay
