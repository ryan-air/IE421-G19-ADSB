#pragma once

// Small, dependency-free hashes.
//  * crc32:  IEEE 802.3 CRC-32 (zlib/PNG compatible). Recording integrity.
//  * Sha256: FIPS 180-4 SHA-256. Determinism hashes of replay output.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace adsb {

// CRC-32 (reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF). Pass the
// previous return value as `crc` to continue a running checksum.
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t crc = 0) noexcept;

class Sha256 {
 public:
  Sha256() noexcept { reset(); }
  void reset() noexcept;
  void update(const void* data, std::size_t n) noexcept;
  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;  // then reset() to reuse
  [[nodiscard]] static std::string hex(const std::array<std::uint8_t, 32>& digest);

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> h_{};
  std::array<std::uint8_t, 64> buf_{};
  std::size_t buf_len_ = 0;
  std::uint64_t total_len_ = 0;
};

}  // namespace adsb
