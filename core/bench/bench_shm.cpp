// Cost of the shared-memory ring's operations on one thread (no contention):
// what the publisher pays per message, and what a reader pays per message.
// For cross-thread latency, see tools/adsb_shm_bench.cpp.

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>

#include "adsb/feed/shm_ring.hpp"

namespace {

using namespace adsb::feed;

struct Ring {
  std::uint64_t cap;
  void* mem;
  RingView view;
  explicit Ring(std::uint64_t c)
      : cap(c), mem(std::aligned_alloc(4096, (shm_region_bytes(c) + 4095) / 4096 * 4096)), view(mem, c) {
    view.format(adsb::protocol::make_session("BENCH00001"), 1, 1, 1);
  }
  ~Ring() { std::free(mem); }
};

void BM_ShmRingWrite(benchmark::State& state) {
  Ring r(1 << 16);
  void* mem = r.mem;
  benchmark::DoNotOptimize(mem);  // the region escapes: its stores cannot be dropped
  std::uint8_t msg[64] = {'U'};
  std::uint64_t seq = 1;
  for (auto _ : state) {
    r.view.write(seq++, msg, sizeof msg);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_ShmRingWrite);

void BM_ShmRingRead(benchmark::State& state) {
  Ring r(1 << 16);
  void* mem = r.mem;
  benchmark::DoNotOptimize(mem);
  std::uint8_t msg[64] = {'U'};
  for (std::uint64_t s = 1; s <= r.cap; ++s) r.view.write(s, msg, sizeof msg);
  std::uint8_t out[64];
  std::size_t len = 0;
  std::uint64_t seq = 1;
  for (auto _ : state) {
    benchmark::DoNotOptimize(r.view.read(seq, out, len));
    benchmark::DoNotOptimize(out);
    seq = seq == r.cap ? 1 : seq + 1;
  }
}
BENCHMARK(BM_ShmRingRead);

}  // namespace
