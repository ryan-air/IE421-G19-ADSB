#include "adsb/modes/crc24.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "test_util.hpp"

namespace {

using adsb::modes::crc24;
using adsb::modes::crc24_bitwise;
using adsb::modes::crc24_syndrome;
using adsb::test::hex;

// Real DF17 frames from "The 1090 MHz Riddle" and the pyModeS documentation.
const char* const kValidDf17[] = {
    "8D4840D6202CC371C32CE0576098",  // identification, KLM1023
    "8D406B902015A678D4D220AA4BDA",  // identification, EZY85MH
    "8D40621D58C382D690C8AC2863A7",  // airborne position, even
    "8D40621D58C386435CC412692AD6",  // airborne position, odd
    "8D40058B58C901375147EFD09357",  // airborne position, even
    "8D40058B58C904A87F402D3B8C59",  // airborne position, odd
    "8D485020994409940838175B284F",  // velocity, ground speed
    "8DA05F219B06B6AF189400CBC33F",  // velocity, airspeed
};

TEST(Crc24, KnownFramesHaveZeroSyndrome) {
  for (const char* s : kValidDf17) {
    const auto f = hex(s);
    ASSERT_EQ(f.size(), 14u);
    EXPECT_EQ(crc24_syndrome(f.data(), f.size()), 0u) << s;
    // Equivalently, the parity field equals the CRC of the first 11 bytes.
    const std::uint32_t parity = (std::uint32_t{f[11]} << 16) | (std::uint32_t{f[12]} << 8) | f[13];
    EXPECT_EQ(crc24(f.data(), 11), parity) << s;
  }
}

TEST(Crc24, TableMatchesBitwiseReference) {
  std::mt19937 rng(123);
  for (int iter = 0; iter < 5'000; ++iter) {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(rng() % 32));
    for (auto& b : data) b = static_cast<std::uint8_t>(rng());
    ASSERT_EQ(crc24(data.data(), data.size()), crc24_bitwise(data.data(), data.size()));
  }
}

TEST(Crc24, EverySingleBitErrorIsDetected) {
  for (const char* s : kValidDf17) {
    auto f = hex(s);
    for (std::size_t bit = 0; bit < 112; ++bit) {
      f[bit / 8] ^= static_cast<std::uint8_t>(0x80u >> (bit % 8));
      EXPECT_NE(crc24_syndrome(f.data(), f.size()), 0u) << s << " bit " << bit;
      f[bit / 8] ^= static_cast<std::uint8_t>(0x80u >> (bit % 8));
    }
  }
}

TEST(Crc24, AddressParityRecoversAddress) {
  // Build a DF4-style short frame whose parity is CRC XOR address, as an
  // aircraft would transmit it; the syndrome must be the address.
  std::mt19937 rng(9);
  for (int iter = 0; iter < 1'000; ++iter) {
    std::uint8_t f[7];
    f[0] = static_cast<std::uint8_t>((4u << 3) | (rng() & 7u));
    for (int i = 1; i < 4; ++i) f[i] = static_cast<std::uint8_t>(rng());
    const std::uint32_t icao = rng() & 0xFFFFFFu;
    const std::uint32_t ap = crc24(f, 4) ^ icao;
    f[4] = static_cast<std::uint8_t>(ap >> 16);
    f[5] = static_cast<std::uint8_t>(ap >> 8);
    f[6] = static_cast<std::uint8_t>(ap);
    ASSERT_EQ(crc24_syndrome(f, 7), icao);
  }
}

TEST(Crc24, EmptyInput) {
  EXPECT_EQ(crc24(nullptr, 0), 0u);
  EXPECT_EQ(crc24_bitwise(nullptr, 0), 0u);
}

}  // namespace
