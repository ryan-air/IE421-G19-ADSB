// adsb-beast-serve: serve a recording as a Beast TCP stream, the way readsb's
// Beast output does, at a controlled rate. It is the load source for the
// overload tests (README_UPDATES.md §10.3): it can push frames faster than a
// consumer can take them, and it reports whether the consumer pushed back.
//
//   adsb-beast-serve FILE.adsbrec [--port 30005] [--bind 127.0.0.1]
//                    [--rate FPS | --speed N] [--burst FPS:AT:SECONDS]
//                    [--loop] [--duration S] [--chunk-us 100]
//
// Pacing: frame k is due at a fixed schedule (--rate: uniform FPS, optionally
// raised to the burst rate between AT and AT + SECONDS; --speed: the
// recording's own timing, N times faster). Every --chunk-us the frames that
// are due are sent in one write, like readsb's output buffer.
//
// Backpressure: the socket is blocking. A consumer that stops reading (once
// both kernel buffers are full) makes send() block, and the server then falls
// behind its schedule: "lateness" and the longest single send() report that.
// (Total time inside send() is mostly the system call itself: about 30 µs per
// call on macOS loopback.)
// A consumer that drops at its own input boundary never causes either.

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/ingest/beast.hpp"
#include "adsb/record/recording.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-beast-serve FILE.adsbrec [options]\n"
               "  --port P              listen port (default 30005, readsb's Beast output port)\n"
               "  --bind HOST           listen address (default 127.0.0.1)\n"
               "  --rate FPS            send frames at a uniform rate (frames per second)\n"
               "  --speed N             or: the recording's own timing, N times faster (default 1)\n"
               "  --burst FPS:AT:SECS   with --rate: switch to FPS from AT for SECS seconds, then back\n"
               "  --loop                restart the recording at the end (with --duration)\n"
               "  --duration S          stop after S seconds of sending\n"
               "  --chunk-us N          send what is due every N microseconds (default 100)\n"
               "Serves one client, then exits. Only line 0 of the recording is sent.\n");
  std::exit(code);
}

struct Burst {
  double fps = 0;
  double at_s = 0;
  double secs = 0;
};

}  // namespace

int main(int argc, char** argv) {
  using namespace adsb;
  std::string path, bind_host = "127.0.0.1";
  std::uint16_t port = 30005;
  double rate = 0, speed = 1, duration = 0;
  std::uint64_t chunk_ns = 100'000;
  bool loop = false;
  Burst burst;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(2);
      return argv[++i];
    };
    if (a == "-h" || a == "--help") usage(0);
    else if (a == "--port") port = static_cast<std::uint16_t>(std::atoi(val().c_str()));
    else if (a == "--bind") bind_host = val();
    else if (a == "--rate") rate = std::atof(val().c_str());
    else if (a == "--speed") speed = std::atof(val().c_str());
    else if (a == "--loop") loop = true;
    else if (a == "--duration") duration = std::atof(val().c_str());
    else if (a == "--chunk-us") chunk_ns = static_cast<std::uint64_t>(std::atof(val().c_str()) * 1000);
    else if (a == "--burst") {
      const std::string b = val();
      if (std::sscanf(b.c_str(), "%lf:%lf:%lf", &burst.fps, &burst.at_s, &burst.secs) != 3) usage(2);
    } else if (!a.empty() && a[0] == '-') usage(2);
    else path = a;
  }
  if (path.empty() || speed <= 0 || rate < 0 || (burst.fps > 0 && rate <= 0) || chunk_ns == 0) usage(2);
  if (loop && duration <= 0) {
    std::fprintf(stderr, "adsb-beast-serve: --loop needs --duration\n");
    return 2;
  }

  // Pre-encode line 0 into one contiguous Beast byte stream.
  record::RecordingReader reader;
  std::string err;
  if (!reader.open(path, &err)) {
    std::fprintf(stderr, "adsb-beast-serve: %s\n", err.c_str());
    return 1;
  }
  std::vector<std::uint8_t> stream;
  std::vector<std::size_t> offset;    // offset[k] = start of frame k; offset[n] = end
  std::vector<std::uint64_t> rx_ns;   // recording timing (for --speed)
  record::Record r;
  while (reader.next(r)) {
    if (r.line != 0) continue;
    ingest::BeastFrame bf;
    bf.type = static_cast<ingest::BeastType>(r.type);
    bf.len = r.len;
    bf.signal = r.signal;
    bf.timestamp = r.radio_ts & 0xFFFFFFFFFFFFULL;
    std::memcpy(bf.payload, r.payload, r.len);
    std::uint8_t buf[ingest::kBeastMaxEncodedLen];
    offset.push_back(stream.size());
    rx_ns.push_back(r.rx_ns);
    const std::size_t n = ingest::beast_encode(bf, buf);
    stream.insert(stream.end(), buf, buf + n);
  }
  const std::size_t frames = rx_ns.size();
  offset.push_back(stream.size());
  if (frames == 0) {
    std::fprintf(stderr, "adsb-beast-serve: no line-0 frames in %s\n", path.c_str());
    return 1;
  }
  const double rec_span_s = static_cast<double>(rx_ns.back() - rx_ns.front()) / 1e9;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  const int lfd = feed::open_tcp_listener(bind_host, port, 1, &err);
  if (lfd < 0) {
    std::fprintf(stderr, "adsb-beast-serve: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "adsb-beast-serve: %zu frames (%.1f s recorded), listening on %s:%u\n", frames,
               rec_span_s, bind_host.c_str(), feed::local_port(lfd));
  int cfd = -1;
  while (!g_stop && cfd < 0) {
    pollfd p{lfd, POLLIN, 0};
    if (::poll(&p, 1, 100) > 0) cfd = ::accept(lfd, nullptr, nullptr);
  }
  feed::close_fd(lfd);
  if (cfd < 0) return g_stop ? 0 : 1;
  // BSD (macOS) accept() inherits the listener's O_NONBLOCK; Linux does not.
  // The server must block in send() when the consumer stops reading: that is
  // the backpressure it measures.
  const int fl = fcntl(cfd, F_GETFL, 0);
  if (fl < 0 || fcntl(cfd, F_SETFL, fl & ~O_NONBLOCK) != 0) {
    std::fprintf(stderr, "adsb-beast-serve: cannot make the connection blocking\n");
    return 1;
  }
  const int one = 1;
  setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  // Schedule. Due times are in ns relative to t0, accumulated per frame so a
  // rate change (burst) applies from the frame where it starts.
  LatencyHistogram lateness;      // how late each chunk went out vs. its first frame's due time
  LatencyHistogram send_blocked;  // time spent inside each send() call
  std::uint64_t sent = 0, bytes = 0, blocked_total_ns = 0, laps = 0;
  bool peer_closed = false;
  const std::uint64_t t0 = mono_ns();
  const std::uint64_t end_ns = duration > 0 ? static_cast<std::uint64_t>(duration * 1e9) : ~std::uint64_t{0};
  double due = 0;  // ns
  std::size_t k = 0;
  auto current_fps = [&](double t_ns) {
    const double t = t_ns / 1e9;
    if (burst.fps > 0 && t >= burst.at_s && t < burst.at_s + burst.secs) return burst.fps;
    return rate;
  };
  auto advance_due = [&]() {
    // Due time of the frame after k (k already sent).
    if (rate > 0) {
      due += 1e9 / current_fps(due);
    } else {
      const std::size_t next = (k + 1) % frames;
      if (next == 0) {
        due += 1e6;  // loop: a 1 ms pause at the wrap
      } else {
        due += static_cast<double>(rx_ns[next] - rx_ns[k]) / speed;
      }
    }
  };

  while (!g_stop && !peer_closed) {
    const std::uint64_t now = mono_ns() - t0;
    if (now >= end_ns) break;
    if (static_cast<std::uint64_t>(due) <= now) {
      // Everything due by now, as contiguous byte ranges (split at a wrap).
      const double chunk_due = due;
      std::size_t first = k;
      std::size_t count = 0;
      while (static_cast<std::uint64_t>(due) <= now && count < frames) {
        advance_due();
        ++count;
        if (++k == frames) {
          if (!loop) break;
          // Send what we have before wrapping (non-contiguous otherwise).
          break;
        }
      }
      lateness.record(now - static_cast<std::uint64_t>(chunk_due));
      const std::uint8_t* p = stream.data() + offset[first];
      std::size_t left = offset[first + count] - offset[first];
      while (left > 0) {
        const std::uint64_t s0 = mono_ns();
        const ssize_t w = ::send(cfd, p, left, 0);
        const std::uint64_t dt = mono_ns() - s0;
        send_blocked.record(dt);
        blocked_total_ns += dt;
        if (w < 0) {
          if (errno == EINTR) continue;
          peer_closed = true;  // EPIPE / ECONNRESET: the consumer went away
          break;
        }
        p += w;
        left -= static_cast<std::size_t>(w);
        bytes += static_cast<std::size_t>(w);
      }
      sent += count;
      if (k == frames) {
        if (!loop) break;
        k = 0;
        ++laps;
      }
      continue;
    }
    // Wait for the next chunk boundary (or the next due frame, if sooner).
    const std::uint64_t wait = std::min<std::uint64_t>(static_cast<std::uint64_t>(due) - now, chunk_ns);
    timespec ts{static_cast<time_t>(wait / 1'000'000'000), static_cast<long>(wait % 1'000'000'000)};
    nanosleep(&ts, nullptr);
  }
  const double secs = static_cast<double>(mono_ns() - t0) / 1e9;
  ::shutdown(cfd, SHUT_WR);
  ::close(cfd);

  auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
  std::printf("sent        %llu frames, %llu bytes in %.3f s (%.0f frames/s), %llu laps%s\n",
              static_cast<unsigned long long>(sent), static_cast<unsigned long long>(bytes), secs,
              static_cast<double>(sent) / std::max(secs, 1e-9), static_cast<unsigned long long>(laps),
              peer_closed ? ", peer closed" : "");
  std::printf("lateness    p50 %.3f  p99 %.3f  max %.3f ms (behind schedule = consumer pushed back)\n",
              ms(lateness.percentile(50)), ms(lateness.percentile(99)), ms(lateness.max()));
  std::printf("send        %.1f ms inside send() in total, longest single send %.3f ms "
              "(a long send = the consumer stopped reading)\n",
              ms(blocked_total_ns), ms(send_blocked.max()));
  return 0;
}
