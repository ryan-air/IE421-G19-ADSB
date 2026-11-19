// adsb-tx-bench: the feed's UDP transports compared on identical histograms
// (README_UPDATES.md §9.3): kernel sendto() vs io_uring with and without a
// kernel submission thread (SQPOLL).
//
//   adsb-tx-bench [--transport kernel|uring|uring-nosqpoll|all] [--rate N] [--seconds S]
//                 [--size B] [--rounds N] [--cpus S,R] [--sq-cpu N] [--in-flight N]
//
// A sender thread sends B-byte datagrams at a fixed rate (busy-wait pacing)
// to a receiver thread on the loopback interface. Per transport it reports:
//   send()      time the hot thread spends in the transport's send call;
//   send->recv  send call start to the receiver's recv() return (same clock);
//   cores       CPU time / wall time, for the sender thread (always about 1:
//               it busy-waits to pace) and the whole process (sender +
//               receiver + io_uring's SQPOLL kernel thread, which belongs to
//               the process: SQPOLL's price is that extra polling core);
//   lost / reordered datagrams at the receiver.
// With --transport all, the transports run in turn, --rounds times, so a
// change in machine load affects all of them alike.
//
// Loopback numbers are not wire numbers: there is no NIC, and the kernel
// delivers each datagram on the sender's CPU. Use the benchmark host (and
// hardware timestamps, §9.4) for TX-to-wire latency.

#include <sys/resource.h>
#include <sys/socket.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "adsb/common/cache.hpp"
#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/common/thread.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/tx.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-tx-bench [options]\n"
               "  --transport T   kernel, uring, uring-nosqpoll, or all (default all)\n"
               "  --rate N        datagrams per second (default 100000)\n"
               "  --seconds S     per transport and round (default 3)\n"
               "  --size B        datagram bytes, 16..2048 (default 1200)\n"
               "  --rounds N      with all: alternate the transports N times (default 1)\n"
               "  --cpus S,R      pin the sender and receiver threads (Linux)\n"
               "  --sq-cpu N      pin io_uring's submission thread (Linux)\n"
               "  --in-flight N   io_uring sends in flight at most (default 64)\n");
  std::exit(code);
}

double cpu_seconds(int who) {
  rusage ru{};
  getrusage(who, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
         static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

#ifdef RUSAGE_THREAD
constexpr int kThread = RUSAGE_THREAD;
#else
constexpr int kThread = RUSAGE_SELF;  // macOS: no per-thread figure
#endif

struct Result {
  Transport t{};
  LatencyHistogram call, e2e;
  std::uint64_t sent = 0, received = 0, lost = 0, reordered = 0, max_late_ns = 0;
  double sender_cpu = 0, process_cpu = 0;
  std::string report;
};

void run(Result& res, double rate, double seconds, std::size_t size, int cpu_s, int cpu_r,
         const UringSender::Options& uopt) {
  std::string err;
  const int rfd = open_udp("127.0.0.1", 0, 16 << 20, &err);
  if (rfd < 0) {
    std::fprintf(stderr, "adsb-tx-bench: receiver: %s\n", err.c_str());
    std::exit(1);
  }
  FeedTx tx;
  if (!tx.open(Endpoint{"127.0.0.1", local_port(rfd)}, res.t, {}, uopt, &err)) {
    std::fprintf(stderr, "adsb-tx-bench: %s: %s\n", transport_name(res.t), err.c_str());
    std::exit(1);
  }
  const auto total = static_cast<std::uint64_t>(rate * seconds);
  std::atomic<bool> ready{false}, done{false};
  std::thread receiver([&] {
    if (cpu_r >= 0) pin_current_thread(cpu_r);
    std::uint8_t buf[4096];
    std::uint64_t expect = 1;
    ready = true;
    for (;;) {
      const ssize_t n = ::recv(rfd, buf, sizeof buf, MSG_DONTWAIT);
      if (n >= 16) {
        const std::uint64_t now = mono_ns();
        std::uint64_t seq, ts;
        std::memcpy(&seq, buf, 8);
        std::memcpy(&ts, buf + 8, 8);
        res.e2e.record(now >= ts ? now - ts : 0);
        ++res.received;
        if (seq < expect) ++res.reordered;
        else expect = seq + 1;
      } else if (done.load(std::memory_order_acquire)) {
        break;
      } else {
        cpu_relax();
      }
    }
  });
  while (!ready.load()) cpu_relax();

  std::thread sender([&] {
    if (cpu_s >= 0) pin_current_thread(cpu_s);
    std::vector<std::uint8_t> d(size, 0xA5);
    const double period = 1e9 / rate;
    const double cpu0 = cpu_seconds(kThread);
    const std::uint64_t t0 = mono_ns() + 1'000'000;
    for (std::uint64_t s = 1; s <= total; ++s) {
      const auto due = t0 + static_cast<std::uint64_t>(static_cast<double>(s - 1) * period);
      std::uint64_t now;
      while ((now = mono_ns()) < due) cpu_relax();
      if (now - due > res.max_late_ns) res.max_late_ns = now - due;
      std::memcpy(d.data(), &s, 8);
      const std::uint64_t start = mono_ns();
      std::memcpy(d.data() + 8, &start, 8);
      (void)tx.send(d);
      res.call.record(mono_ns() - start);
    }
    res.sender_cpu = cpu_seconds(kThread) - cpu0;
  });
  const double proc0 = cpu_seconds(RUSAGE_SELF);
  sender.join();
  (void)tx.drain(2000);
  res.process_cpu = cpu_seconds(RUSAGE_SELF) - proc0;
  // Let the last datagrams arrive.
  const std::uint64_t until = mono_ns() + 200'000'000;
  while (mono_ns() < until) cpu_relax();
  done.store(true, std::memory_order_release);
  receiver.join();
  res.sent = total;
  res.lost = total > res.received ? total - res.received : 0;
  res.report = tx.report();
  close_fd(rfd);
}

}  // namespace

int main(int argc, char** argv) {
  std::string which = "all";
  double rate = 100'000, seconds = 3;
  std::size_t size = 1200;
  int rounds = 1, cpu_s = -1, cpu_r = -1;
  UringSender::Options uopt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(1);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--transport") which = val();
    else if (a == "--rate") rate = std::atof(val().c_str());
    else if (a == "--seconds") seconds = std::atof(val().c_str());
    else if (a == "--size") size = std::strtoul(val().c_str(), nullptr, 10);
    else if (a == "--rounds") rounds = std::atoi(val().c_str());
    else if (a == "--sq-cpu") uopt.sq_cpu = std::atoi(val().c_str());
    else if (a == "--in-flight") uopt.max_in_flight = static_cast<unsigned>(std::strtoul(val().c_str(), nullptr, 10));
    else if (a == "--cpus") {
      if (std::sscanf(val().c_str(), "%d,%d", &cpu_s, &cpu_r) != 2) usage(1);
    } else usage(1);
  }
  if (rate <= 0 || seconds <= 0 || size < 16 || size > UringSender::kBufferBytes || rounds < 1) usage(1);
  std::vector<Transport> ts;
  if (which == "all") {
    ts = {Transport::kKernel, Transport::kUring, Transport::kUringNoSqpoll};
    if (!UringSender::built()) ts = {Transport::kKernel};
  } else {
    Transport t{};
    if (!parse_transport(which, t)) usage(1);
    ts = {t};
  }

  std::vector<Result> all(ts.size());
  for (std::size_t k = 0; k < ts.size(); ++k) all[k].t = ts[k];
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  for (int r = 0; r < rounds; ++r) {
    for (Result& res : all) {
      Result one;
      one.t = res.t;
      run(one, rate, seconds, size, cpu_s, cpu_r, uopt);
      res.call.merge(one.call);
      res.e2e.merge(one.e2e);
      res.sent += one.sent;
      res.received += one.received;
      res.lost += one.lost;
      res.reordered += one.reordered;
      res.max_late_ns = std::max(res.max_late_ns, one.max_late_ns);
      res.sender_cpu += one.sender_cpu;
      res.process_cpu += one.process_cpu;
      res.report = one.report;
    }
  }
  std::printf("rate        %.0f datagrams/s of %zu bytes, %.1f s x %d round(s) per transport, cpus %d,%d, sq cpu %d\n",
              rate, size, seconds, rounds, cpu_s, cpu_r, uopt.sq_cpu);
  for (const Result& res : all) {
    const double wall = seconds * rounds;
    std::printf("%-15s send() p50 %llu p99 %llu p99.9 %llu max %llu ns | send->recv p50 %llu p99 %llu p99.9 %llu ns | "
                "cores busy: sender %.2f, process %.2f | lost %llu, reordered %llu, sender max late %.1f us\n",
                transport_name(res.t), u(res.call.percentile(50)), u(res.call.percentile(99)),
                u(res.call.percentile(99.9)), u(res.call.max()), u(res.e2e.percentile(50)),
                u(res.e2e.percentile(99)), u(res.e2e.percentile(99.9)), res.sender_cpu / wall,
                res.process_cpu / wall, u(res.lost), u(res.reordered),
                static_cast<double>(res.max_late_ns) / 1e3);
    if (!res.report.empty()) std::printf("                %s\n", res.report.c_str());
  }
  return 0;
}
