// adsb-shm-bench: latency of the shared-memory ring alone (README_UPDATES.md
// §2 target: p99 < 500 ns publish -> consumer read on the same host).
//
//   adsb-shm-bench [--rate N] [--seconds S] [--slots N] [--cpus W,R] [--name /adsb-bench]
//
// A writer thread publishes 64-byte messages at a fixed rate (busy-wait
// pacing), each carrying the mono_ns() time just before ring.write(); a reader
// thread spins on the ring and records mono_ns() - that time for every message
// it reads. Both use a real POSIX shared-memory object (same mapping semantics
// as two processes). No publisher, book, or network: this is the transport's
// own cost, to compare with the end-to-end numbers of adsb-shm-cli.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/common/thread.hpp"
#include "adsb/feed/shm_ring.hpp"

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-shm-bench [options]\n"
               "  --rate N       messages per second (default 100000)\n"
               "  --seconds S    duration (default 5)\n"
               "  --slots N      ring size, a power of two (default 65536)\n"
               "  --cpus W,R     pin the writer and reader threads (Linux only)\n"
               "  --name NAME    shared-memory object (default /adsb-bench)\n");
  std::exit(code);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace adsb;
  using namespace adsb::feed;
  double rate = 100'000, seconds = 5;
  std::uint64_t slots = kShmDefaultSlots;
  int cpu_w = -1, cpu_r = -1;
  std::string name = "/adsb-bench";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--rate") rate = std::atof(val().c_str());
    else if (a == "--seconds") seconds = std::atof(val().c_str());
    else if (a == "--slots") slots = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--name") name = val();
    else if (a == "--cpus") {
      if (std::sscanf(val().c_str(), "%d,%d", &cpu_w, &cpu_r) != 2) usage(1);
    } else usage(1);
  }
  if (rate <= 0 || seconds <= 0) usage(1);

  ShmRegion region;
  std::string err;
  if (!region.create(name, slots, protocol::make_session("SHMBENCH01"), 1, &err)) {
    std::fprintf(stderr, "adsb-shm-bench: %s\n", err.c_str());
    return 1;
  }
  ShmRegion reader_map;  // a second, read-only mapping, as another process would have
  if (!reader_map.open(name, &err)) {
    std::fprintf(stderr, "adsb-shm-bench: %s\n", err.c_str());
    return 1;
  }
  RingView w = region.view();
  const RingView r = reader_map.view();
  const auto total = static_cast<std::uint64_t>(rate * seconds);
  std::atomic<bool> reader_ready{false};
  LatencyHistogram lat;
  std::uint64_t laps = 0, read_count = 0;

  std::thread reader([&] {
    if (cpu_r >= 0) pin_current_thread(cpu_r);
    std::uint8_t msg[kShmMaxMessage];
    std::size_t len = 0;
    std::uint64_t expect = 1;
    reader_ready = true;
    while (expect <= total) {
      const RingView::Read res = r.read(expect, msg, len);
      if (res == RingView::Read::kOk) {
        const std::uint64_t now = mono_ns();
        std::uint64_t ts;
        std::memcpy(&ts, msg + 56, 8);
        lat.record(now >= ts ? now - ts : 0);
        ++read_count;
        ++expect;
      } else if (res == RingView::Read::kLapped) {
        ++laps;
        expect = r.resume_after_lap(expect);
      } else {
        cpu_relax();
      }
    }
  });
  if (cpu_w >= 0) pin_current_thread(cpu_w);
  while (!reader_ready.load()) cpu_relax();

  std::uint8_t msg[kShmMaxMessage] = {'U'};
  const double period_ns = 1e9 / rate;
  const std::uint64_t t0 = mono_ns() + 1'000'000;
  std::uint64_t max_late = 0;
  for (std::uint64_t s = 1; s <= total; ++s) {
    const auto due = t0 + static_cast<std::uint64_t>(static_cast<double>(s - 1) * period_ns);
    std::uint64_t now;
    while ((now = mono_ns()) < due) cpu_relax();
    if (now - due > max_late) max_late = now - due;
    std::memcpy(msg + 4, &s, 8);
    const std::uint64_t ts = mono_ns();
    std::memcpy(msg + 56, &ts, 8);
    w.write(s, msg, sizeof msg);
  }
  reader.join();

  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("rate        %.0f messages/s for %.1f s, %llu-slot ring, cpus %d,%d\n", rate, seconds, u(slots), cpu_w, cpu_r);
  std::printf("read        %llu of %llu, %llu laps, writer max lateness %.1f us\n", u(read_count), u(total), u(laps),
              static_cast<double>(max_late) / 1e3);
  std::printf("latency     write -> read: p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  p99.99 %llu  max %llu ns\n",
              u(lat.percentile(50)), u(lat.percentile(90)), u(lat.percentile(99)), u(lat.percentile(99.9)),
              u(lat.percentile(99.99)), u(lat.max()));
  return 0;
}
