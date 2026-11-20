// adsb-dst: deterministic simulation testing of the feed (feed/dst.hpp).
//
//   adsb-dst [--seeds N] [--start S] [--threads T] [--profile quick|deep]
//            [--time-limit SECONDS] [--keep-going]
//   adsb-dst --seed S [--profile quick|deep] [--trace]
//
// Sweep: runs seeds S, S+1, ... S+N-1 (each its own random world) on T
// threads and stops at the first failure (or keeps going and lists the
// first few). Prints totals over all runs, so a sweep shows it exercised
// every recovery path, and a sweep hash that depends only on the seeds and
// the code: the same sweep gives the same hash on any machine and compiler.
// --time-limit stops starting new seeds after that long (nightly budgets);
// the hash then covers only the seeds that ran.
//
// One seed: prints its world and the result; --trace prints every event.
// A failing seed from a sweep replays exactly this way.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adsb/feed/dst.hpp"

namespace {

using namespace adsb::feed;

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-dst [--seeds N] [--start S] [--threads T] [--profile quick|deep]\n"
               "                [--time-limit SECONDS] [--keep-going]\n"
               "       adsb-dst --seed S [--profile quick|deep] [--trace]\n"
               "  --seeds N        number of seeds to run (default 1000)\n"
               "  --start S        first seed (default 1)\n"
               "  --threads T      worker threads (default: hardware threads)\n"
               "  --profile P      quick (small worlds, default) or deep (larger, longer)\n"
               "  --time-limit S   stop starting seeds after S seconds\n"
               "  --keep-going     do not stop at the first failure (lists up to 10)\n"
               "  --seed S         run one seed and print its world and result\n"
               "  --trace          with --seed: print every event\n");
  std::exit(code);
}

void print_totals(const dst::Totals& t) {
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf(
      "totals      %llu messages published in %llu sessions, %llu datagrams (%llu lost, %llu duplicated), "
      "%llu recovery stalls (%llu blocked the publisher), %llu queue overflows\n"
      "subscribers %llu applied, %llu gaps, %llu requests (%llu retried), %llu NAKs, %llu snapshots loaded "
      "(%llu failed), %llu session changes, %llu stale packets, %llu window overflows, %llu truncated replies, %llu duplicates\n"
      "checks      %llu invariant evaluations\n",
      u(t.published), u(t.sessions), u(t.datagrams), u(t.lost), u(t.duplicated), u(t.stalls), u(t.publisher_blocked), u(t.queue_overflow),
      u(t.applied), u(t.gaps), u(t.requests), u(t.request_retries), u(t.naks), u(t.snapshots_loaded),
      u(t.snapshot_failures), u(t.session_changes), u(t.stale_packets), u(t.window_overflow), u(t.truncated_replies), u(t.duplicates),
      u(t.checks));
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t seeds = 1000, start = 1, one = 0;
  bool single = false, trace = false, keep_going = false;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  double time_limit = 0;
  dst::Profile profile = dst::Profile::kQuick;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--seeds") seeds = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--start") start = std::strtoull(val().c_str(), nullptr, 10);
    else if (a == "--threads") threads = static_cast<unsigned>(std::strtoul(val().c_str(), nullptr, 10));
    else if (a == "--time-limit") time_limit = std::strtod(val().c_str(), nullptr);
    else if (a == "--keep-going") keep_going = true;
    else if (a == "--seed") {
      one = std::strtoull(val().c_str(), nullptr, 10);
      single = true;
    } else if (a == "--trace") trace = true;
    else if (a == "--profile") {
      const std::string p = val();
      if (p == "quick") profile = dst::Profile::kQuick;
      else if (p == "deep") profile = dst::Profile::kDeep;
      else usage(1);
    } else usage(1);
  }
  if (threads == 0 || (!single && seeds == 0)) usage(1);

  if (single) {
    const dst::Config cfg = dst::random_config(one, profile);
    std::printf("%s", dst::describe(cfg).c_str());
    dst::TraceFn fn = [](std::uint64_t now, const std::string& line) {
      std::printf("%14.6f ms  %s\n", static_cast<double>(now) / 1e6, line.c_str());
    };
    const dst::Result r = dst::run(cfg, trace ? &fn : nullptr);
    std::printf("result      %s after %" PRIu64 " events, %.3f ms simulated, trace hash %016" PRIx64 "\n",
                r.ok ? "PASS" : "FAIL", r.events, static_cast<double>(r.sim_ns) / 1e6, r.trace_hash);
    if (!r.ok) {
      std::printf("failure     at %.6f ms: %s\n", static_cast<double>(r.failure_ns) / 1e6, r.failure.c_str());
    }
    print_totals(r.totals);
    return r.ok ? 0 : 1;
  }

  // The sweep hash is a sum over seeds of a mix of (seed, run hash):
  // independent of which thread ran which seed, and no per-seed storage
  // (nightly sweeps ask for huge seed counts under a time limit).
  auto mix = [](std::uint64_t seed, std::uint64_t h) {
    std::uint64_t z = h ^ (seed * 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  std::atomic<std::uint64_t> sweep_sum{0};
  std::atomic<std::uint64_t> next{0}, done{0};
  std::atomic<bool> stop{false};
  std::mutex mu;
  dst::Totals totals;
  std::uint64_t events = 0;
  std::vector<std::pair<std::uint64_t, dst::Result>> failures;
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

  auto worker = [&] {
    dst::Totals mine;
    std::uint64_t my_events = 0;
    for (;;) {
      if (stop.load(std::memory_order_relaxed)) break;
      if (time_limit > 0 && elapsed() > time_limit) break;
      const std::uint64_t k = next.fetch_add(1);
      if (k >= seeds) break;
      const std::uint64_t seed = start + k;
      const dst::Result r = dst::run(dst::random_config(seed, profile));
      sweep_sum.fetch_add(mix(seed, r.trace_hash), std::memory_order_relaxed);
      mine.add(r.totals);
      my_events += r.events;
      if (!r.ok) {
        std::lock_guard<std::mutex> g(mu);
        // Keep the 10 lowest failing seeds (not the first 10 to finish), so
        // the report does not depend on thread timing.
        failures.emplace_back(seed, r);
        std::sort(failures.begin(), failures.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (failures.size() > 10) failures.pop_back();
        if (!keep_going) stop = true;
      }
      const std::uint64_t d = done.fetch_add(1) + 1;
      if (d % 10000 == 0) {
        std::lock_guard<std::mutex> g(mu);
        std::printf("progress    %llu seeds, %.1f s\n", static_cast<unsigned long long>(d), elapsed());
        std::fflush(stdout);
      }
    }
    std::lock_guard<std::mutex> g(mu);
    totals.add(mine);
    events += my_events;
  };
  std::vector<std::thread> pool;
  for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  const std::uint64_t sweep = sweep_sum.load();
  const std::uint64_t count = done.load();
  const double secs = elapsed();
  std::printf("sweep       %llu seeds from %llu (%s profile) on %u threads in %.1f s: %.0f seeds/s, %.1f M events\n",
              static_cast<unsigned long long>(count), static_cast<unsigned long long>(start),
              profile == dst::Profile::kDeep ? "deep" : "quick", threads, secs,
              static_cast<double>(count) / std::max(secs, 1e-9), static_cast<double>(events) / 1e6);
  print_totals(totals);
  std::printf("sweep hash  %016" PRIx64 "%s\n", sweep, count == seeds ? "" : " (partial sweep)");
  std::sort(failures.begin(), failures.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [seed, r] : failures) {
    std::printf("\nFAIL seed %llu at %.6f ms: %s\n", static_cast<unsigned long long>(seed),
                static_cast<double>(r.failure_ns) / 1e6, r.failure.c_str());
    std::printf("%s", dst::describe(dst::random_config(seed, profile)).c_str());
    std::printf("replay: adsb-dst --seed %llu%s --trace\n", static_cast<unsigned long long>(seed),
                profile == dst::Profile::kDeep ? " --profile deep" : "");
  }
  if (!failures.empty()) return 1;
  std::printf("result      PASS\n");
  return 0;
}
