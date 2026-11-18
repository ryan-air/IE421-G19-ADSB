// adsb-ring-stress: long concurrent stress runs of the two lock-free rings
// (README_UPDATES.md §12, memory-model testing). Run it for minutes or hours
// on ARM (weak memory ordering, where reordering bugs show up) and on x86.
//
//   adsb-ring-stress [--mode spsc|shm|both] [--seconds S] [--seed N]
//
// SPSC queue (adsb/common/spsc_ring.hpp), at capacity 4 (constantly full and
// empty) and 1024: one producer mixing try_push and try_push_n, one consumer
// mixing try_pop, try_pop_n and front/pop, both with random jitter. Every
// message must arrive exactly once, in order, with intact contents.
//
// Shared-memory ring (adsb/feed/shm_ring.hpp), 64 slots: one writer at full
// speed with jitter, three readers (fast, jittery, slow). Every message a
// reader accepts must be intact and in sequence order; laps must be reported,
// never papered over.
//
// Prints counts per mode and exits 1 on any violation.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "adsb/common/cache.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/common/spsc_ring.hpp"
#include "adsb/feed/shm_ring.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// 64 bytes, every word derived from seq: a torn or mixed copy is detectable.
struct Msg {
  std::uint64_t seq;
  std::uint64_t w[7];
};
static_assert(sizeof(Msg) == 64);

Msg make(std::uint64_t seq) {
  Msg m{};
  m.seq = seq;
  for (std::uint64_t i = 0; i < 7; ++i) m.w[i] = seq * 0x9E3779B97F4A7C15ULL ^ (i << 56) ^ 0xA5;
  return m;
}
bool intact(const Msg& m) {
  for (std::uint64_t i = 0; i < 7; ++i) {
    if (m.w[i] != (m.seq * 0x9E3779B97F4A7C15ULL ^ (i << 56) ^ 0xA5)) return false;
  }
  return true;
}

void jitter(adsb::Rng& rng, std::uint64_t one_in) {
  if (rng.below(one_in) == 0) {
    const std::uint64_t n = rng.below(200);
    for (std::uint64_t k = 0; k < n; ++k) adsb::cpu_relax();
  }
}

struct Result {
  std::uint64_t ok = 0;
  std::uint64_t violations = 0;
  std::uint64_t laps = 0;
};

// ---------------------------------------------------------------------------
template <std::size_t Cap>
Result stress_spsc(double seconds, std::uint64_t seed) {
  auto ring = std::make_unique<adsb::SpscRing<Msg, Cap>>();
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> produced{0};
  Result res;
  std::thread consumer([&] {
    adsb::Rng rng(seed * 2 + 1);
    std::uint64_t expect = 1;
    Msg batch[32];
    auto check = [&](const Msg& m) {
      if (m.seq != expect || !intact(m)) ++res.violations;
      expect = m.seq + 1;
      ++res.ok;
    };
    for (;;) {
      const std::uint64_t mode = rng.below(3);
      if (mode == 0) {
        Msg m;
        if (ring->try_pop(m)) check(m);
      } else if (mode == 1) {
        const std::size_t want = 1 + rng.below(32);
        const std::size_t n = ring->try_pop_n(std::span<Msg>(batch, want));
        for (std::size_t i = 0; i < n; ++i) check(batch[i]);
      } else if (const Msg* f = ring->front()) {
        check(*f);
        ring->pop();
      }
      jitter(rng, 64);
      if (stop.load(std::memory_order_acquire) && expect > produced.load(std::memory_order_acquire)) break;
    }
  });
  adsb::Rng rng(seed * 2);
  std::uint64_t next = 1;
  Msg batch[32];
  const auto end = Clock::now() + std::chrono::duration<double>(seconds);
  std::uint64_t iter = 0;
  while ((++iter & 1023) != 0 || Clock::now() < end) {
    if (rng.below(2) == 0) {
      const Msg m = make(next);
      if (ring->try_push(m)) ++next;
    } else {
      const std::size_t want = 1 + rng.below(32);
      for (std::size_t i = 0; i < want; ++i) batch[i] = make(next + i);
      next += ring->try_push_n(std::span<const Msg>(batch, want));
    }
    produced.store(next - 1, std::memory_order_release);
    jitter(rng, 64);
  }
  stop.store(true, std::memory_order_release);
  consumer.join();
  if (res.ok != next - 1) ++res.violations;  // every message exactly once
  return res;
}

// ---------------------------------------------------------------------------
Result stress_shm(double seconds, std::uint64_t seed) {
  using adsb::feed::RingView;
  constexpr std::uint64_t kCap = 64;
  const std::size_t bytes = (adsb::feed::shm_region_bytes(kCap) + 4095) / 4096 * 4096;
  void* mem = std::aligned_alloc(4096, bytes);
  RingView ring(mem, kCap);
  ring.format(adsb::protocol::make_session("STRESS0001"), 1, 1, 1);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> last_written{0};
  std::vector<Result> per(3);
  std::vector<std::thread> readers;
  for (std::size_t t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      adsb::Rng rng(seed * 10 + t);
      std::uint64_t expect = 1, last = 0;
      std::uint8_t buf[64];
      std::size_t len = 0;
      for (;;) {
        const RingView::Read r = ring.read(expect, buf, len);
        if (r == RingView::Read::kOk) {
          Msg m;
          std::memcpy(&m, buf, sizeof m);
          if (m.seq != expect || !intact(m) || len != sizeof m || expect <= last) ++per[t].violations;
          last = expect;
          ++per[t].ok;
          ++expect;
          if (t == 1) jitter(rng, 8);
          if (t == 2) jitter(rng, 1);
        } else if (r == RingView::Read::kLapped) {
          ++per[t].laps;
          // next_seq() may lag the lap just seen (see resume_after_lap); the
          // order check on the next kOk catches any move backwards.
          expect = ring.resume_after_lap(expect);
        } else if (stop.load(std::memory_order_acquire) && expect > last_written.load(std::memory_order_acquire)) {
          break;
        }
      }
    });
  }
  adsb::Rng rng(seed);
  const auto end = Clock::now() + std::chrono::duration<double>(seconds);
  std::uint64_t seq = 0;
  while ((seq & 1023) != 0 || Clock::now() < end) {
    ++seq;
    const Msg m = make(seq);
    ring.write(seq, reinterpret_cast<const std::uint8_t*>(&m), sizeof m);
    last_written.store(seq, std::memory_order_release);
    jitter(rng, 256);
  }
  stop.store(true, std::memory_order_release);
  for (auto& th : readers) th.join();
  std::free(mem);
  Result total;
  for (const Result& r : per) {
    total.ok += r.ok;
    total.violations += r.violations;
    total.laps += r.laps;
  }
  if (per[2].laps == 0) {
    std::printf("warning     the slow reader was never lapped: lap detection not exercised\n");
  }
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "both";
  double seconds = 10;
  std::uint64_t seed = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) std::exit(2);
      return argv[++i];
    };
    if (a == "--mode") mode = val();
    else if (a == "--seconds") seconds = std::atof(val().c_str());
    else if (a == "--seed") seed = std::strtoull(val().c_str(), nullptr, 10);
    else {
      std::fprintf(stderr, "usage: adsb-ring-stress [--mode spsc|shm|both] [--seconds S] [--seed N]\n");
      return 2;
    }
  }
  if (mode != "spsc" && mode != "shm" && mode != "both") return 2;
  const bool both = mode == "both";
  const double share = both ? seconds / 3 : seconds / (mode == "spsc" ? 2 : 1);
  std::uint64_t violations = 0;
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  if (mode != "shm") {
    const Result a = stress_spsc<4>(share, seed);
    std::printf("spsc cap 4     %llu messages, %llu violations\n", u(a.ok), u(a.violations));
    const Result b = stress_spsc<1024>(share, seed + 1);
    std::printf("spsc cap 1024  %llu messages, %llu violations\n", u(b.ok), u(b.violations));
    violations += a.violations + b.violations;
  }
  if (mode != "spsc") {
    const Result c = stress_shm(share, seed + 2);
    std::printf("shm 64 slots   %llu messages read, %llu laps, %llu violations\n", u(c.ok), u(c.laps), u(c.violations));
    violations += c.violations;
  }
  std::printf("%s\n", violations == 0 ? "OK" : "FAILED");
  return violations == 0 ? 0 : 1;
}
