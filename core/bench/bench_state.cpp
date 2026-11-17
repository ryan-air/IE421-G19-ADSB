// Track store and engine benchmarks: FlatTrackStore vs std::unordered_map
// (README_UPDATES.md §8.3), and per-frame decode + state cost.

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/sim/synth.hpp"
#include "adsb/state/track_engine.hpp"
#include "adsb/state/track_store.hpp"

namespace {

using namespace adsb;
using namespace adsb::state;

// Keys: a mix of a dense national block (as real traffic has) and random ones.
std::vector<std::uint32_t> make_keys(std::size_t n, std::uint64_t seed) {
  Rng rng(seed);
  std::vector<std::uint32_t> keys;
  while (keys.size() < n) {
    keys.push_back(keys.size() % 2 == 0 ? 0xA00000 + static_cast<std::uint32_t>(rng.below(0x40000))
                                        : static_cast<std::uint32_t>(rng.below(0xFFFFFF)));
  }
  return keys;
}

template <typename Store>
std::unique_ptr<Store> filled(const std::vector<std::uint32_t>& keys) {
  auto s = std::make_unique<Store>();
  for (const auto k : keys) {
    if (!s->find(k)) (void)s->insert(k);
  }
  return s;
}

template <typename Store>
void BM_LookupHit(benchmark::State& state) {
  const auto keys = make_keys(static_cast<std::size_t>(state.range(0)), 1);
  auto s = filled<Store>(keys);
  // Visit keys in a scrambled order so the access pattern is not sequential.
  std::vector<std::uint32_t> order = keys;
  Rng rng(2);
  for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng.below(i)]);
  std::size_t i = 0;
  for (auto _ : state) {
    auto t = s->find(order[i]);
    benchmark::DoNotOptimize(t.hot);
    if (++i == order.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

template <typename Store>
void BM_LookupMiss(benchmark::State& state) {
  const auto keys = make_keys(static_cast<std::size_t>(state.range(0)), 1);
  auto s = filled<Store>(keys);
  // Addresses guaranteed absent from the store.
  std::vector<std::uint32_t> misses;
  for (const auto k : make_keys(16384, 99)) {
    if (!s->find(k)) misses.push_back(k);
    if (misses.size() == 4096) break;
  }
  std::size_t i = 0;
  for (auto _ : state) {
    auto t = s->find(misses[i]);
    benchmark::DoNotOptimize(t.hot);
    i = (i + 1) & 4095;
  }
  state.SetItemsProcessed(state.iterations());
}

// Insert + erase churn at a steady population (aircraft arriving / leaving).
template <typename Store>
void BM_Churn(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = make_keys(2 * n, 3);
  auto s = filled<Store>(std::vector<std::uint32_t>(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(n)));
  std::size_t out = 0, in = n;
  for (auto _ : state) {
    s->erase(keys[out]);
    if (!s->find(keys[in])) benchmark::DoNotOptimize(s->insert(keys[in]).hot);
    out = (out + 1) % keys.size();
    in = (in + 1) % keys.size();
  }
  state.SetItemsProcessed(state.iterations());
}

struct NaiveStore8192 : NaiveTrackStore {
  NaiveStore8192() : NaiveTrackStore(8192) {}
};
using Flat = FlatTrackStore<8192>;

BENCHMARK(BM_LookupHit<Flat>)->Arg(100)->Arg(1000)->Arg(4000);
BENCHMARK(BM_LookupHit<NaiveStore8192>)->Arg(100)->Arg(1000)->Arg(4000);
BENCHMARK(BM_LookupMiss<Flat>)->Arg(1000)->Arg(4000);
BENCHMARK(BM_LookupMiss<NaiveStore8192>)->Arg(1000)->Arg(4000);
BENCHMARK(BM_Churn<Flat>)->Arg(1000)->Arg(4000);
BENCHMARK(BM_Churn<NaiveStore8192>)->Arg(1000)->Arg(4000);

// Full per-frame cost: decode + dedup + field updates + CPR + update emission,
// over realistic synthetic traffic (300 aircraft, two receiver lines).
template <typename Engine>
void BM_EngineOnFrame(benchmark::State& state) {
  sim::SynthConfig cfg;
  cfg.aircraft = 300;
  cfg.duration_s = 30;
  cfg.lines = 2;
  const auto s = sim::generate(cfg);
  struct NullSink {
    std::uint64_t n = 0;
    void on_update(const protocol::TrackUpdate& u) { n += u.icao; }
    void on_delete(const protocol::TrackDelete& d) { n += d.icao; }
  } sink;
  std::uint64_t frames = 0;
  for (auto _ : state) {
    state.PauseTiming();
    auto e = std::make_unique<Engine>();
    state.ResumeTiming();
    for (const auto& r : s.records) {
      FrameMeta m;
      m.rx_ns = r.rx_ns;
      m.radio_ts = r.radio_ts;
      m.line = r.line;
      e->on_frame(r.payload, r.len, m, sink);
    }
    frames += s.records.size();
  }
  benchmark::DoNotOptimize(sink.n);
  state.SetItemsProcessed(static_cast<std::int64_t>(frames));
  state.counters["ns_per_frame"] = benchmark::Counter(
      static_cast<double>(frames), benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_EngineOnFrame<TrackEngine>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_EngineOnFrame<NaiveTrackEngine>)->Unit(benchmark::kMillisecond);

}  // namespace
