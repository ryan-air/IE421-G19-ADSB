// SPSC ring microbenchmarks.
//
//  * BM_SpscSingleThread: push+pop cost with no contention (instruction cost).
//  * BM_SpscThroughput:   producer and consumer on two threads, 64-byte
//                         messages; reports items/s. Pin threads (taskset /
//                         isolcpus) on Linux for stable numbers.
//  * BM_SpscPingPong:     round trip through two rings; half of it is the
//                         one-way cross-core handoff latency.

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "adsb/common/cache.hpp"
#include "adsb/common/spsc_ring.hpp"

namespace {

struct Msg64 {
  std::uint64_t words[8];
};

void BM_SpscSingleThread(benchmark::State& state) {
  auto ring = std::make_unique<adsb::SpscRing<Msg64, 1024>>();
  Msg64 in{};
  Msg64 out{};
  for (auto _ : state) {
    in.words[0]++;
    benchmark::DoNotOptimize(ring->try_push(in));
    benchmark::DoNotOptimize(ring->try_pop(out));
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscSingleThread);

template <std::size_t Capacity>
void BM_SpscThroughput(benchmark::State& state) {
  auto ring = std::make_unique<adsb::SpscRing<Msg64, Capacity>>();
  constexpr std::uint64_t kBatch = 1 << 20;
  for (auto _ : state) {
    std::thread consumer([&] {
      Msg64 m{};
      std::uint64_t got = 0;
      while (got < kBatch) {
        if (ring->try_pop(m)) {
          ++got;
        } else {
          adsb::cpu_relax();
        }
      }
      benchmark::DoNotOptimize(m);
    });
    Msg64 m{};
    for (std::uint64_t i = 0; i < kBatch;) {
      m.words[0] = i;
      if (ring->try_push(m)) {
        ++i;
      } else {
        adsb::cpu_relax();
      }
    }
    consumer.join();
  }
  const auto iters = static_cast<std::uint64_t>(state.iterations());
  state.SetItemsProcessed(static_cast<std::int64_t>(iters * kBatch));
  state.SetBytesProcessed(static_cast<std::int64_t>(iters * kBatch * sizeof(Msg64)));
}
BENCHMARK(BM_SpscThroughput<256>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscThroughput<4096>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscThroughput<65536>)->UseRealTime()->Unit(benchmark::kMillisecond);

void BM_SpscPingPong(benchmark::State& state) {
  auto ping = std::make_unique<adsb::SpscRing<std::uint64_t, 64>>();
  auto pong = std::make_unique<adsb::SpscRing<std::uint64_t, 64>>();
  std::atomic<bool> stop{false};
  std::thread echo([&] {
    std::uint64_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (ping->try_pop(v)) {
        while (!pong->try_push(v)) adsb::cpu_relax();
      } else {
        adsb::cpu_relax();
      }
    }
  });
  std::uint64_t seq = 0;
  for (auto _ : state) {
    while (!ping->try_push(seq)) adsb::cpu_relax();
    std::uint64_t back = 0;
    while (!pong->try_pop(back)) adsb::cpu_relax();
    benchmark::DoNotOptimize(back);
    ++seq;
  }
  stop.store(true);
  echo.join();
  state.counters["one_way_ns"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * 2.0,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_SpscPingPong)->UseRealTime();

}  // namespace
