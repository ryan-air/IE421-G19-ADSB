// Fuzzes the Beast stream parser with arbitrary bytes and arbitrary chunking.
//
// Invariants:
//  * no crash, no out-of-bounds access (ASan), no UB (UBSan);
//  * every emitted frame has the payload length its type requires;
//  * the parser accounts for every input byte;
//  * re-encoding the emitted frames and parsing them again (in one chunk)
//    yields exactly the same frames, with no resync events.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "adsb/ingest/beast.hpp"
#include "fuzz_check.hpp"

using namespace adsb::ingest;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  // First byte picks the chunk size so split frames are exercised.
  const std::size_t chunk = 1 + data[0] % 64;
  ++data;
  --size;

  BeastParser parser;
  std::vector<BeastFrame> frames;
  auto sink = [&](const BeastFrame& f) {
    FUZZ_CHECK(f.len == beast_payload_len(static_cast<std::uint8_t>(f.type)));
    FUZZ_CHECK(f.timestamp < (std::uint64_t{1} << 48));
    for (std::size_t i = f.len; i < sizeof(f.payload); ++i) FUZZ_CHECK(f.payload[i] == 0);
    frames.push_back(f);
  };
  for (std::size_t off = 0; off < size; off += chunk) {
    const std::size_t n = size - off < chunk ? size - off : chunk;
    parser.feed(data + off, n, sink);
  }
  FUZZ_CHECK(parser.stats().bytes == size);
  FUZZ_CHECK(parser.stats().frames == frames.size());

  // Round trip: encode -> parse must reproduce the frames exactly.
  std::vector<std::uint8_t> wire;
  for (const auto& f : frames) {
    std::uint8_t buf[kBeastMaxEncodedLen];
    const std::size_t n = beast_encode(f, buf);
    FUZZ_CHECK(n <= kBeastMaxEncodedLen);
    wire.insert(wire.end(), buf, buf + n);
  }
  BeastParser again;
  std::size_t idx = 0;
  again.feed(wire.data(), wire.size(), [&](const BeastFrame& f) {
    FUZZ_CHECK(idx < frames.size());
    const BeastFrame& g = frames[idx++];
    FUZZ_CHECK(f.type == g.type && f.len == g.len && f.signal == g.signal &&
               f.timestamp == g.timestamp);
    FUZZ_CHECK(std::memcmp(f.payload, g.payload, sizeof(f.payload)) == 0);
  });
  FUZZ_CHECK(idx == frames.size());
  FUZZ_CHECK(again.stats().truncated == 0 && again.stats().bad_type == 0 &&
             again.stats().skipped_bytes == 0);
  return 0;
}
