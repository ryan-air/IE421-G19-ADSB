// Fuzzes the MoldUDP64 packet parser and the subscriber sequence tracker.
//
// Invariants:
//  * parse_packet never reads outside the datagram (ASan);
//  * an accepted packet's blocks exactly cover the body, and for_each visits
//    `count` messages with consecutive sequence numbers;
//  * rebuilding an accepted data packet reproduces it byte for byte;
//  * SequenceTracker's `expected` never moves backwards.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "adsb/protocol/moldudp64.hpp"
#include "fuzz_check.hpp"

using namespace adsb::protocol;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Treat the input as a sequence of datagrams: [u16 length][bytes]...
  SequenceTracker tracker(1);
  std::size_t off = 0;
  while (off + 2 <= size) {
    std::uint16_t len;
    std::memcpy(&len, data + off, 2);
    off += 2;
    if (len > size - off) len = static_cast<std::uint16_t>(size - off);
    const std::span<const std::uint8_t> dgram(data + off, len);
    off += len;

    const auto p = parse_packet(dgram);
    if (!p) continue;
    FUZZ_CHECK(dgram.size() >= PacketHeader::kWireSize);

    std::uint64_t expect_seq = p->header.seq;
    std::size_t visited = 0, covered = 0;
    p->for_each([&](std::uint64_t seq, std::span<const std::uint8_t> msg) {
      FUZZ_CHECK(seq == expect_seq++);
      FUZZ_CHECK(msg.data() >= dgram.data() && msg.data() + msg.size() <= dgram.data() + dgram.size());
      covered += 2 + msg.size();
      ++visited;
    });
    if (p->kind == PacketKind::kData) {
      FUZZ_CHECK(visited == p->header.count);
      FUZZ_CHECK(covered == p->body.size());
      // Rebuild and compare (only if it fits the builder's MTU-sized buffer).
      if (dgram.size() <= PacketBuilder::kCapacity) {
        PacketBuilder b(p->header.session, p->header.seq);
        bool ok = true;
        p->for_each([&](std::uint64_t, std::span<const std::uint8_t> msg) {
          ok = ok && b.add(msg.data(), static_cast<std::uint16_t>(msg.size()));
        });
        if (ok) {
          const auto out = b.finish();
          FUZZ_CHECK(out.size() == dgram.size());
          FUZZ_CHECK(std::memcmp(out.data(), dgram.data(), out.size()) == 0);
        }
      }
    } else {
      FUZZ_CHECK(visited == 0 && p->body.empty());
    }

    const std::uint64_t before = tracker.expected();
    const auto o = tracker.on_packet(*p);
    if (o.result == SequenceTracker::Result::kNewSession) {
      tracker.reset(p->header.session, p->header.seq);
    } else {
      FUZZ_CHECK(tracker.expected() >= before);
      if (o.result == SequenceTracker::Result::kGap) {
        FUZZ_CHECK(o.gap_from == before && o.gap_to > o.gap_from);
        tracker.advance_to(p->header.seq + p->header.count);
      }
    }
  }
  return 0;
}
