#include "adsb/ingest/beast.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "test_util.hpp"

namespace {

using namespace adsb::ingest;
using adsb::test::hex;

std::vector<BeastFrame> parse_all(BeastParser& p, const std::vector<std::uint8_t>& bytes) {
  std::vector<BeastFrame> out;
  p.feed(bytes.data(), bytes.size(), [&](const BeastFrame& f) { out.push_back(f); });
  return out;
}

bool same_frame(const BeastFrame& a, const BeastFrame& b) {
  return a.type == b.type && a.len == b.len && a.signal == b.signal &&
         a.timestamp == b.timestamp && std::memcmp(a.payload, b.payload, a.len) == 0;
}

BeastFrame make_frame(BeastType type, std::uint64_t ts, std::uint8_t signal,
                      const std::vector<std::uint8_t>& payload) {
  BeastFrame f;
  f.type = type;
  f.len = static_cast<std::uint8_t>(payload.size());
  f.timestamp = ts;
  f.signal = signal;
  std::memcpy(f.payload, payload.data(), payload.size());
  return f;
}

std::vector<std::uint8_t> encode(const BeastFrame& f) {
  std::vector<std::uint8_t> buf(kBeastMaxEncodedLen);
  buf.resize(beast_encode(f, buf.data()));
  return buf;
}

TEST(Beast, ParsesLongFrame) {
  // Hand-built wire bytes: 1A '3' ts(6) signal(1) payload(14).
  std::vector<std::uint8_t> wire = {0x1A, '3', 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x80};
  const auto payload = hex("8D4840D6202CC371C32CE0576098");
  wire.insert(wire.end(), payload.begin(), payload.end());

  BeastParser p;
  const auto frames = parse_all(p, wire);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].type, BeastType::kModeSLong);
  EXPECT_EQ(frames[0].len, 14);
  EXPECT_EQ(frames[0].timestamp, 0x010203040506u);
  EXPECT_EQ(frames[0].signal, 0x80);
  EXPECT_EQ(std::memcmp(frames[0].payload, payload.data(), 14), 0);
  EXPECT_EQ(p.stats().frames, 1u);
  EXPECT_EQ(p.stats().skipped_bytes, 0u);
}

TEST(Beast, AllFrameTypes) {
  BeastParser p;
  std::vector<std::uint8_t> wire;
  const std::vector<BeastFrame> in = {
      make_frame(BeastType::kModeAC, 1, 2, {0x12, 0x34}),
      make_frame(BeastType::kModeSShort, 3, 4, hex("5D4840D6A1B2C3")),
      make_frame(BeastType::kModeSLong, 5, 6, hex("8D4840D6202CC371C32CE0576098")),
      make_frame(BeastType::kStatus, 7, 8, std::vector<std::uint8_t>(14, 0x55)),
  };
  for (const auto& f : in) {
    const auto e = encode(f);
    wire.insert(wire.end(), e.begin(), e.end());
  }
  const auto out = parse_all(p, wire);
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) EXPECT_TRUE(same_frame(in[i], out[i])) << i;
}

TEST(Beast, UnescapesDoubled1A) {
  // 0x1A in the timestamp, signal, and payload must each be sent as 1A 1A.
  const auto f = make_frame(BeastType::kModeSShort, 0x1A001A1A0000ULL, 0x1A,
                            {0x1A, 0x00, 0x1A, 0x1A, 0x01, 0x02, 0x1A});
  const auto wire = encode(f);
  // 2 header bytes + 7 + 7 body bytes + one extra byte per 0x1A (3 + 1 + 4).
  EXPECT_EQ(wire.size(), 2u + 14u + 8u);
  BeastParser p;
  const auto out = parse_all(p, wire);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(same_frame(f, out[0]));
}

TEST(Beast, ByteAtATimeAndRandomChunking) {
  // A long mixed stream split at every possible boundary must parse identically.
  std::mt19937_64 rng(31);
  std::vector<BeastFrame> in;
  std::vector<std::uint8_t> wire;
  for (int i = 0; i < 2'000; ++i) {
    static constexpr BeastType kTypes[] = {BeastType::kModeAC, BeastType::kModeSShort,
                                           BeastType::kModeSLong, BeastType::kStatus};
    const BeastType t = kTypes[rng() % 4];
    std::vector<std::uint8_t> payload(beast_payload_len(static_cast<std::uint8_t>(t)));
    // Bias toward 0x1A so escaping is exercised heavily.
    for (auto& b : payload) b = (rng() % 4 == 0) ? 0x1A : static_cast<std::uint8_t>(rng());
    const std::uint64_t ts = (rng() % 3 == 0) ? 0x1A1A1A1A1A1AULL : (rng() & 0xFFFFFFFFFFFFULL);
    in.push_back(make_frame(t, ts, static_cast<std::uint8_t>(rng()), payload));
    const auto e = encode(in.back());
    wire.insert(wire.end(), e.begin(), e.end());
  }

  for (const int mode : {0, 1}) {
    BeastParser p;
    std::vector<BeastFrame> out;
    auto sink = [&](const BeastFrame& f) { out.push_back(f); };
    std::size_t i = 0;
    while (i < wire.size()) {
      const std::size_t chunk = mode == 0 ? 1 : 1 + static_cast<std::size_t>(rng() % 97);
      const std::size_t n = std::min(chunk, wire.size() - i);
      p.feed(wire.data() + i, n, sink);
      i += n;
    }
    ASSERT_EQ(out.size(), in.size()) << "mode " << mode;
    for (std::size_t k = 0; k < in.size(); ++k) ASSERT_TRUE(same_frame(in[k], out[k])) << k;
    EXPECT_EQ(p.stats().truncated, 0u);
    EXPECT_EQ(p.stats().bad_type, 0u);
    EXPECT_EQ(p.stats().bytes, wire.size());
  }
}

TEST(Beast, ResyncsAfterGarbageAndUnknownTypes) {
  const auto good = make_frame(BeastType::kModeSShort, 42, 9, hex("5D4840D6A1B2C3"));
  const auto good_wire = encode(good);

  std::vector<std::uint8_t> wire = {0x00, 0xFF, 0x13, 0x37};  // leading garbage
  wire.insert(wire.end(), {0x1A, 0x39, 0x01, 0x02});           // unknown type '9'
  wire.insert(wire.end(), {0x1A, 0x1A, 0x05});                 // escaped 1A while hunting
  wire.insert(wire.end(), good_wire.begin(), good_wire.end());

  BeastParser p;
  const auto out = parse_all(p, wire);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(same_frame(good, out[0]));
  EXPECT_EQ(p.stats().bad_type, 1u);
  EXPECT_GT(p.stats().skipped_bytes, 0u);
}

TEST(Beast, TruncatedFrameIsDroppedAndNextFrameParsed) {
  // A frame cut off mid-body by the start of the next frame.
  const auto good = make_frame(BeastType::kModeSLong, 7, 1, hex("8D4840D6202CC371C32CE0576098"));
  const auto good_wire = encode(good);
  std::vector<std::uint8_t> wire = {0x1A, '3', 0x00, 0x00, 0x00, 0x00, 0x00};  // 5 of 21 body bytes
  wire.insert(wire.end(), good_wire.begin(), good_wire.end());

  BeastParser p;
  const auto out = parse_all(p, wire);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(same_frame(good, out[0]));
  EXPECT_EQ(p.stats().truncated, 1u);
}

TEST(Beast, ResetDiscardsPartialFrame) {
  const auto good = make_frame(BeastType::kModeAC, 1, 1, {0x01, 0x02});
  const auto wire = encode(good);
  BeastParser p;
  std::size_t n = 0;
  auto sink = [&](const BeastFrame&) { ++n; };
  p.feed(wire.data(), wire.size() - 1, sink);  // all but the last byte
  p.reset();
  p.feed(wire.data() + wire.size() - 1, 1, sink);  // stray final byte: ignored
  EXPECT_EQ(n, 0u);
  p.feed(wire.data(), wire.size(), sink);
  EXPECT_EQ(n, 1u);
}

TEST(Beast, ShortPayloadIsZeroPadded) {
  BeastParser p;
  std::vector<BeastFrame> out;
  // A long frame followed by a short frame: the short frame's payload tail
  // must not leak bytes from the previous long frame.
  for (const auto& f : {make_frame(BeastType::kModeSLong, 1, 1, std::vector<std::uint8_t>(14, 0xEE)),
                        make_frame(BeastType::kModeAC, 2, 2, {0x01, 0x02})}) {
    const auto w = encode(f);
    p.feed(w.data(), w.size(), [&](const BeastFrame& fr) { out.push_back(fr); });
  }
  ASSERT_EQ(out.size(), 2u);
  for (std::size_t i = 2; i < 14; ++i) EXPECT_EQ(out[1].payload[i], 0) << i;
}

}  // namespace
