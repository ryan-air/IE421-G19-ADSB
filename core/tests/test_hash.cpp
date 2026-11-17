#include "adsb/common/hash.hpp"
#include "adsb/common/rng.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using adsb::Sha256;

std::string sha_hex(const std::string& s) {
  Sha256 h;
  h.update(s.data(), s.size());
  return Sha256::hex(h.finish());
}

TEST(Crc32, StandardCheckValue) {
  // The CRC-32/ISO-HDLC check value for "123456789".
  EXPECT_EQ(adsb::crc32("123456789", 9), 0xCBF43926u);
  EXPECT_EQ(adsb::crc32("", 0), 0u);
}

TEST(Crc32, IncrementalEqualsOneShot) {
  const std::string s = "The quick brown fox jumps over the lazy dog";
  const std::uint32_t whole = adsb::crc32(s.data(), s.size());
  EXPECT_EQ(whole, 0x414FA339u);
  std::uint32_t part = adsb::crc32(s.data(), 10);
  part = adsb::crc32(s.data() + 10, s.size() - 10, part);
  EXPECT_EQ(part, whole);
}

TEST(Sha256, Fips180Vectors) {
  EXPECT_EQ(sha_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // One million 'a', fed in uneven chunks to exercise block boundaries.
  Sha256 h;
  const std::string chunk(997, 'a');
  std::size_t left = 1'000'000;
  while (left > 0) {
    const std::size_t n = left < chunk.size() ? left : chunk.size();
    h.update(chunk.data(), n);
    left -= n;
  }
  EXPECT_EQ(Sha256::hex(h.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ResetReuses) {
  Sha256 h;
  h.update("abc", 3);
  (void)h.finish();
  h.reset();
  h.update("abc", 3);
  EXPECT_EQ(Sha256::hex(h.finish()), sha_hex("abc"));
}

TEST(Rng, DeterministicAndInRange) {
  adsb::Rng a(42), b(42), c(43);
  bool differs = false;
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t x = a.next();
    ASSERT_EQ(x, b.next());
    if (x != c.next()) differs = true;
  }
  EXPECT_TRUE(differs);

  adsb::Rng r(7);
  double sum = 0;
  for (int i = 0; i < 100'000; ++i) {
    const double u = r.uniform();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
    sum += u;
    ASSERT_LT(r.below(10), 10u);
  }
  EXPECT_NEAR(sum / 100'000, 0.5, 0.01);
}

TEST(Rng, KnownFirstOutput) {
  // Pins the generator so recordings made from a seed stay reproducible
  // across releases and platforms. If this changes, regenerate fixtures.
  adsb::Rng r(1);
  const std::uint64_t first = r.next();
  adsb::Rng again(1);
  EXPECT_EQ(again.next(), first);
  EXPECT_EQ(first, 0xB3F2AF6D0FC710C5ULL);
}

}  // namespace
