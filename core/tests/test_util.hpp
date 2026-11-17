#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace adsb::test {

// Parses a hex string such as "8D4840D6202CC371C32CE0576098" into bytes.
inline std::vector<std::uint8_t> hex(std::string_view s) {
  auto nibble = [](char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    throw std::invalid_argument("bad hex digit");
  };
  if (s.size() % 2 != 0) throw std::invalid_argument("odd-length hex string");
  std::vector<std::uint8_t> out;
  out.reserve(s.size() / 2);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>((nibble(s[i]) << 4) | nibble(s[i + 1])));
  }
  return out;
}

}  // namespace adsb::test
