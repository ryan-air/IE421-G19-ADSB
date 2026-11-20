// adsb-tx-bench: the feed's UDP transports compared on identical histograms
// (README_UPDATES.md §9.3): kernel sendto() vs io_uring with and without a
// kernel submission thread (SQPOLL) vs AF_XDP.
//
//   adsb-tx-bench [--transport all|T[,T...]] [--rate N] [--seconds S] [--size B]
//                 [--rounds N] [--cpus S,R] [--interface ADDR --dest ADDR [--rx-netns NS]]
//                 [--rx kernel|xdp|all]
//                 [transport flags: --uring-sq-cpu N, --uring-in-flight N, --xdp-mode M, ...]
//
// A sender thread sends B-byte datagrams at a fixed rate (busy-wait pacing)
// to a receiver thread, by default over loopback. AF_XDP needs a real device:
// --interface is the sending device's address, --dest the receiver's. With
// --rx-netns the receiver's socket is created in that network namespace
// (e.g. the far end of a veth pair), so every transport crosses the same
// device; otherwise kernel UDP to a local address would short-cut through
// the loopback device. Per transport it reports:
//   send()      time the hot thread spends in the transport's send call;
//   send->recv  send call start to the receiver's recv() return (same clock);
//   cores       CPU time / wall time, for the sender thread (always about 1:
//               it busy-waits to pace) and the whole process (sender +
//               receiver + io_uring's SQPOLL kernel thread, which belongs to
//               the process: SQPOLL's price is that extra polling core);
//   lost / reordered datagrams at the receiver.
// --rx picks the receive path: the kernel socket (recv()), AF_XDP (an XDP
// program on --dest's device redirects the datagrams into an XSK the
// receiver thread polls; needs CAP_NET_ADMIN, CAP_BPF, CAP_NET_RAW and a
// real device, so --interface/--dest), or both, crossed with every transport.
// --xdp-mode and --xdp-queue apply to the AF_XDP receiver as well as to the
// AF_XDP sender (on a multi-queue NIC, steer the flow to that queue or set
// the device to one queue: frames on other queues reach only the kernel).
// With --transport all, the transports run in turn, --rounds times, so a
// change in machine load affects all of them alike.
//
// Loopback numbers are not wire numbers: there is no NIC, and the kernel
// delivers each datagram on the sender's CPU. Use the benchmark host (and
// hardware timestamps, §9.4) for TX-to-wire latency.

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
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
#include "adsb/feed/xdp_receiver.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-tx-bench [options]\n"
               "  --transport T   all (default: kernel, uring, uring-nosqpoll, and xdp with --interface)\n"
               "                  or a comma-separated list of kernel, uring, uring-nosqpoll, xdp\n"
               "  --rate N        datagrams per second (default 100000)\n"
               "  --seconds S     per transport and round (default 3)\n"
               "  --size B        datagram bytes, 16..2048 (default 1200)\n"
               "  --rounds N      with all: alternate the transports N times (default 1)\n"
               "  --cpus S,R      pin the sender and receiver threads (Linux)\n"
               "  --interface A   the sending device's address (needed for xdp)\n"
               "  --dest A        the receiver's address (default 127.0.0.1)\n"
               "  --rx-netns NS   create the receiver socket in network namespace NS (Linux)\n"
               "  --rx R          receive path: kernel (default), xdp (AF_XDP on --dest's device), or all\n"
               "transport flags (as in adsb-publish):\n" ADSB_TX_OPTIONS_HELP);
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
  bool xdp_rx = false;
  LatencyHistogram call, e2e;
  std::uint64_t sent = 0, received = 0, lost = 0, reordered = 0, max_late_ns = 0;
  double sender_cpu = 0, process_cpu = 0;
  std::string report;
};

// A receiver socket, created in network namespace `netns` if given (a socket
// stays in the namespace it was created in, whichever thread uses it), and
// with `xdp` an AF_XDP receiver for the same port opened there too (its
// device lookup, socket, map and program all belong to that namespace). The
// receiver takes --xdp-mode and --xdp-queue from the sender's options, so
// `--xdp-mode zerocopy` asks for zero-copy at both ends.
int open_receiver(const std::string& host, const std::string& netns, const XdpSender::Options& xo,
                  XdpReceiver* xdp, std::string* err) {
  auto open_all = [&]() -> int {
    const int fd = open_udp(host, 0, 16 << 20, err);
    if (fd < 0 || !xdp) return fd;
    XdpReceiver::Options o;
    o.interface_host = host;
    o.mode = xo.mode;
    o.queue = xo.queue;
    if (!xdp->open(Endpoint{host, local_port(fd)}, o, err)) {
      close_fd(fd);
      return -1;
    }
    return fd;
  };
  if (netns.empty()) return open_all();
#if defined(__linux__)
  int fd = -1;
  std::thread t([&] {
    const int ns = ::open(("/var/run/netns/" + netns).c_str(), O_RDONLY | O_CLOEXEC);
    if (ns < 0 || ::setns(ns, CLONE_NEWNET) != 0) {
      *err = "enter netns " + netns + ": " + std::strerror(errno);
      if (ns >= 0) ::close(ns);
      return;
    }
    ::close(ns);
    fd = open_all();
  });
  t.join();
  return fd;
#else
  *err = "--rx-netns is Linux only";
  return -1;
#endif
}

struct Setup {
  double rate = 100'000, seconds = 3;
  std::size_t size = 1200;
  int cpu_s = -1, cpu_r = -1;
  std::string iface, dest = "127.0.0.1", netns;
  TxOptions txo;
};

void run(Result& res, const Setup& st) {
  const double rate = st.rate, seconds = st.seconds;
  const std::size_t size = st.size;
  const int cpu_s = st.cpu_s, cpu_r = st.cpu_r;
  std::string err;
  XdpReceiver xr;
  const int rfd = open_receiver(st.dest, st.netns, st.txo.xdp, res.xdp_rx ? &xr : nullptr, &err);
  if (rfd < 0) {
    std::fprintf(stderr, "adsb-tx-bench: receiver: %s\n", err.c_str());
    std::exit(1);
  }
  const Endpoint to{st.dest, local_port(rfd)};
  UdpSender::Options udp;
  if (!st.iface.empty()) udp.interface_host = st.iface;
  if (res.t == Transport::kXdp && st.txo.xdp.dst_mac.empty()) {
    // Make sure the kernel has resolved the receiver's MAC (ARP).
    UdpSender probe;
    InterfaceInfo dev;
    MacAddress mac{};
    if (probe.open(Endpoint{st.dest, 9}, udp, nullptr) && interface_for_address(st.iface, dev)) {
      const std::uint8_t b[1] = {0};
      for (int i = 0; i < 100 && !arp_lookup(st.dest, dev.name, mac); ++i) {
        (void)probe.send(b);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }
  FeedTx tx;
  if (!tx.open(to, res.t, udp, st.txo, &err)) {
    std::fprintf(stderr, "adsb-tx-bench: %s: %s\n", transport_name(res.t), err.c_str());
    std::exit(1);
  }
  const auto total = static_cast<std::uint64_t>(rate * seconds);
  std::atomic<bool> ready{false}, done{false};
  std::thread receiver([&] {
    if (cpu_r >= 0) pin_current_thread(cpu_r);
    std::uint8_t buf[4096];
    std::uint64_t expect = 1;
    auto got = [&](std::span<const std::uint8_t> d) {
      if (d.size() < 16) return;
      const std::uint64_t now = mono_ns();
      std::uint64_t seq, ts;
      std::memcpy(&seq, d.data(), 8);
      std::memcpy(&ts, d.data() + 8, 8);
      res.e2e.record(now >= ts ? now - ts : 0);
      ++res.received;
      if (seq < expect) ++res.reordered;
      else expect = seq + 1;
    };
    ready = true;
    for (;;) {
      bool any = false;
      if (xr.is_open()) {
        any = xr.receive(got, 64) != 0;
      } else {
        const ssize_t n = ::recv(rfd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
          got(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
          any = true;
        }
      }
      if (any) continue;
      if (done.load(std::memory_order_acquire)) break;
      cpu_relax();
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
  if (xr.is_open()) {
    xr.refresh_kernel_stats();
    const auto& x = xr.stats();
    char line[256];
    std::snprintf(line, sizeof line, "%saf_xdp rx %s/%s: %llu received, %llu rejected, kernel drops %llu (ring full %llu)",
                  res.report.empty() ? "" : "; ", XdpSender::mode_name(xr.bound_mode()),
                  XdpReceiver::attach_name(xr.attach_mode()), static_cast<unsigned long long>(x.received),
                  static_cast<unsigned long long>(x.rejected), static_cast<unsigned long long>(x.rx_dropped),
                  static_cast<unsigned long long>(x.rx_ring_full));
    res.report += line;
  }
  close_fd(rfd);
}

}  // namespace

int main(int argc, char** argv) {
  std::string which = "all";
  Setup st;
  double& rate = st.rate;
  double& seconds = st.seconds;
  std::size_t& size = st.size;
  int rounds = 1;
  std::string rx = "kernel";
  Transport ignored{};
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
    else if (a == "--interface") st.iface = val();
    else if (a == "--dest") st.dest = val();
    else if (a == "--rx-netns") st.netns = val();
    else if (a == "--rx") rx = val();
    else if (is_tx_flag(a)) { if (!apply_tx_flag(a, val(), ignored, st.txo)) usage(1); }
    else if (a == "--cpus") {
      if (std::sscanf(val().c_str(), "%d,%d", &st.cpu_s, &st.cpu_r) != 2) usage(1);
    } else usage(1);
  }
  if (rate <= 0 || seconds <= 0 || size < 16 || size > UringSender::kBufferBytes || rounds < 1) usage(1);
  std::vector<Transport> ts;
  if (which == "all") {
    ts = {Transport::kKernel};
    if (UringSender::built()) ts.insert(ts.end(), {Transport::kUring, Transport::kUringNoSqpoll});
    if (XdpSender::built() && !st.iface.empty()) ts.push_back(Transport::kXdp);
  } else {
    std::size_t from = 0;
    while (from <= which.size()) {
      const std::size_t comma = std::min(which.find(',', from), which.size());
      Transport t{};
      if (!parse_transport(which.substr(from, comma - from), t)) usage(1);
      ts.push_back(t);
      from = comma + 1;
    }
  }
  if (std::find(ts.begin(), ts.end(), Transport::kXdp) != ts.end() && st.iface.empty()) {
    std::fprintf(stderr, "adsb-tx-bench: xdp needs --interface (the sending device's address) and --dest\n");
    return 1;
  }

  if (rx != "kernel" && rx != "xdp" && rx != "all") usage(1);
  if (rx != "kernel" && (st.iface.empty() || !XdpReceiver::built())) {
    std::fprintf(stderr, "adsb-tx-bench: --rx xdp needs AF_XDP and a device: --interface and --dest\n");
    return 1;
  }
  std::vector<Result> all;
  for (const Transport t : ts) {
    for (const bool x : {false, true}) {
      if ((x && rx == "kernel") || (!x && rx == "xdp")) continue;
      Result r;
      r.t = t;
      r.xdp_rx = x;
      all.push_back(std::move(r));
    }
  }
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  for (int r = 0; r < rounds; ++r) {
    for (Result& res : all) {
      Result one;
      one.t = res.t;
      one.xdp_rx = res.xdp_rx;
      run(one, st);
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
  std::printf("rate        %.0f datagrams/s of %zu bytes, %.1f s x %d round(s) per transport, cpus %d,%d, sq cpu %d, "
              "to %s%s%s\n",
              rate, size, seconds, rounds, st.cpu_s, st.cpu_r, st.txo.uring.sq_cpu, st.dest.c_str(),
              st.netns.empty() ? "" : " in netns ", st.netns.c_str());
  for (const Result& res : all) {
    const double wall = seconds * rounds;
    const std::string name = std::string(transport_name(res.t)) + (res.xdp_rx ? " > xdp" : " > kernel");
    std::printf("%-22s send() p50 %llu p99 %llu p99.9 %llu max %llu ns | send->recv p50 %llu p99 %llu p99.9 %llu ns | "
                "cores busy: sender %.2f, process %.2f | lost %llu, reordered %llu, sender max late %.1f us\n",
                name.c_str(), u(res.call.percentile(50)), u(res.call.percentile(99)),
                u(res.call.percentile(99.9)), u(res.call.max()), u(res.e2e.percentile(50)),
                u(res.e2e.percentile(99)), u(res.e2e.percentile(99.9)), res.sender_cpu / wall,
                res.process_cpu / wall, u(res.lost), u(res.reordered),
                static_cast<double>(res.max_late_ns) / 1e3);
    if (!res.report.empty()) std::printf("                       %s\n", res.report.c_str());
  }
  return 0;
}
