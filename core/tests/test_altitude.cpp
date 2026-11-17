#include "adsb/modes/altitude.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <map>
#include <vector>

namespace {

using namespace adsb::modes;

TEST(Altitude, Ac12QBitKnownVectors) {
  // ME altitude field of 8D40621D58C382D690C8AC2863A7 -> 38000 ft.
  ASSERT_TRUE(decode_ac12(0xC38).has_value());
  EXPECT_EQ(*decode_ac12(0xC38), 38000);
  // 8D40058B58C901375147EFD09357 -> 39000 ft.
  EXPECT_EQ(decode_ac12(0xC90).value_or(-1), 39000);
}

TEST(Altitude, Ac12QBitFullRange) {
  // With Q = 1 every 11-bit N maps to 25 N - 1000 ft.
  for (unsigned n = 0; n < 2048; ++n) {
    // Insert Q (bit 4) into the 11-bit value: high 7 bits, Q, low 4 bits.
    const auto ac12 = static_cast<std::uint16_t>(((n & 0x7F0u) << 1) | 0x10u | (n & 0xFu));
    const auto alt = decode_ac12(ac12);
    ASSERT_TRUE(alt.has_value()) << n;
    EXPECT_EQ(*alt, static_cast<std::int32_t>(n) * 25 - 1000);
  }
}

TEST(Altitude, Ac13QBitMatchesAc12) {
  // A 13-bit AC field is the 12-bit field with M = 0 inserted at bit 6.
  for (unsigned ac12 = 1; ac12 < 4096; ++ac12) {
    const auto ac13 = static_cast<std::uint16_t>(((ac12 & 0xFC0u) << 1) | (ac12 & 0x3Fu));
    EXPECT_EQ(decode_ac12(static_cast<std::uint16_t>(ac12)), decode_ac13(ac13)) << ac12;
  }
}

TEST(Altitude, NoAltitudeAndMetric) {
  EXPECT_FALSE(decode_ac12(0).has_value());
  EXPECT_FALSE(decode_ac13(0).has_value());
  EXPECT_FALSE(decode_ac13(0x0040 | 0x0010 | 0x0001).has_value()) << "M = 1 is metric";
}

TEST(Altitude, ModeAPermutation) {
  // id13_to_mode_a is a bit permutation of the 12 code bits (X dropped).
  EXPECT_EQ(id13_to_mode_a(0x0000), 0x0000);
  EXPECT_EQ(id13_to_mode_a(0x0040), 0x0000) << "X / M bit is not part of the code";
  // Each of the 12 code bits maps to a distinct Mode A bit.
  std::uint16_t seen = 0;
  for (int b = 0; b < 13; ++b) {
    if (b == 6) continue;
    const std::uint16_t a = id13_to_mode_a(static_cast<std::uint16_t>(1u << b));
    ASSERT_EQ(std::popcount(a), 1) << b;
    ASSERT_EQ(seen & a, 0) << b;
    seen |= a;
  }
  EXPECT_EQ(seen, 0x7777) << "all four octal digits fully covered";
}

TEST(Altitude, SquawkFromIdField) {
  // Squawk 7700 = A=7, B=7, C=0, D=0: A4 A2 A1 B4 B2 B1 set.
  // ID13 bit positions: A1=0x800 A2=0x200 A4=0x080 B1=0x020 B2=0x008 B4=0x002.
  EXPECT_EQ(id13_to_mode_a(0x800 | 0x200 | 0x080 | 0x020 | 0x008 | 0x002), 0x7700);
  // Squawk 1200 = A1, B2.
  EXPECT_EQ(id13_to_mode_a(0x800 | 0x008), 0x1200);
  // Squawk 0001 = D1 (in the Q position).
  EXPECT_EQ(id13_to_mode_a(0x010), 0x0001);
}

TEST(Altitude, GillhamIsAGrayCodeOverItsWholeRange) {
  // Enumerate every 12-bit Mode A code with D1 = 0. Valid codes must cover
  // -1200 ft .. 126700 ft in 100 ft steps, each altitude exactly once, and
  // codes for adjacent altitudes must differ in exactly one bit (the defining
  // property of the Gillham Gray code). This validates the decoder without
  // trusting a hand-written encoder.
  std::map<std::int32_t, std::uint16_t> by_alt;
  for (unsigned raw = 0; raw < (1u << 12); ++raw) {
    // Spread 12 bits over the Mode A layout (nibbles A, B, C, D, 3 bits each).
    const auto a = static_cast<std::uint16_t>(((raw & 0xE00u) << 3) | ((raw & 0x1C0u) << 2) |
                                              ((raw & 0x038u) << 1) | (raw & 0x007u));
    if (a & 0x0001) continue;  // D1 is never used for altitude
    const auto alt = gillham_to_feet(a);
    if (!alt) continue;
    ASSERT_EQ(*alt % 100, 0);
    ASSERT_TRUE(by_alt.emplace(*alt, a).second) << "altitude " << *alt << " decoded twice";
  }
  ASSERT_FALSE(by_alt.empty());
  EXPECT_EQ(by_alt.begin()->first, -1200);
  EXPECT_EQ(by_alt.rbegin()->first, 126700);
  EXPECT_EQ(by_alt.size(), static_cast<std::size_t>((126700 + 1200) / 100 + 1));

  std::int32_t prev_alt = 0;
  std::uint16_t prev_code = 0;
  bool first = true;
  for (const auto& [alt, code] : by_alt) {
    if (!first) {
      EXPECT_EQ(alt - prev_alt, 100);
      EXPECT_EQ(std::popcount(static_cast<unsigned>(code ^ prev_code)), 1)
          << prev_alt << " -> " << alt;
    }
    first = false;
    prev_alt = alt;
    prev_code = code;
  }
}

TEST(Altitude, GillhamThroughAc13) {
  // -1200 ft is the code with only C4 set (Mode A 0x0040); in the AC13 field
  // C4 is bit 8 (0x100) and Q must be 0.
  EXPECT_EQ(decode_ac13(0x0100).value_or(0), -1200);
  // Invalid: no C bits set.
  EXPECT_FALSE(decode_ac13(0x0800).has_value());
}

}  // namespace
