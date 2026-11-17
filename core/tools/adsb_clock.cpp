// adsb-clock: answers clock queries on stdin/stdout, so a process in another
// language can map its own monotonic clock onto ours.
//
// TrackUpdate.publish_ts_ns is the publisher's mono_ns() (CLOCK_MONOTONIC_RAW).
// Node's process.hrtime() reads a different clock (CLOCK_MONOTONIC on Linux,
// mach_absolute_time on macOS), so the WebSocket gateway cannot subtract the
// two directly. It runs this helper, writes one byte per query, and reads back
// one line with mono_ns(); taking the sample with the shortest round trip gives
// the offset to within half that round trip (tens of microseconds).
//
//   adsb-clock          one decimal line per byte read; exits on EOF

#include <unistd.h>

#include <cstdio>

#include "adsb/common/clock.hpp"

int main() {
  char c;
  char line[32];
  while (::read(0, &c, 1) == 1) {
    const int n = std::snprintf(line, sizeof line, "%llu\n",
                                static_cast<unsigned long long>(adsb::mono_ns()));
    if (n <= 0 || ::write(1, line, static_cast<std::size_t>(n)) != n) return 1;
  }
  return 0;
}
