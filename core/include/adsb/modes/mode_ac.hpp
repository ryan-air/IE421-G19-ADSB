#pragma once

// Mode A/C replies (ICAO Annex 10 Vol IV 3.1.1, RTCA DO-181): the pulse
// train and the 16-bit code word used for it here and by readsb / dump1090.
//
// A reply is up to 20 pulse slots 1.45 us apart, each pulse 0.45 us wide,
// counted from the leading edge of the first framing pulse F1:
//
//   slot   0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
//   pulse  F1 C1 A1 C2 A2 C4 A4 X  B1 D1 B2 D2 B4 D4 F2 -  -  SPI -  -
//
// F2 is at 20.3 us, the optional SPI (special position identification,
// "ident") pulse 4.35 us after F2, i.e. at 24.65 us. X and the slots marked
// "-" are always empty. A Mode A reply carries the identity (squawk), a
// Mode C reply the pressure altitude in the Gillham code (altitude.hpp); the
// pulses do not say which interrogation was answered.
//
// Code word (readsb's mm->msg[0..1], the 2-byte payload of a Beast type '1'
// frame, big-endian): the squawk digits as hex nibbles plus SPI,
//
//   bit  15 14 13 12 11 10  9  8  7   6  5  4  3  2  1  0
//        0  A4 A2 A1  0 B4 B2 B1 SPI C4 C2 C1  0 D4 D2 D1
//
// so squawk 7700 is 0x7700 and 1200 with ident is 0x1280. This is the
// layout of id13_to_mode_a(), extended by SPI at 0x0080.

#include <cstdint>

namespace adsb::modes {

inline constexpr double kModeAcSlotUs = 1.45;    // pulse spacing
inline constexpr double kModeAcPulseUs = 0.45;   // pulse width
inline constexpr int kModeAcSlots = 20;
inline constexpr int kModeAcF2Slot = 14;         // 20.3 us
inline constexpr int kModeAcXSlot = 7;
inline constexpr int kModeAcSpiSlot = 17;        // 24.65 us
inline constexpr double kModeAcF2Us = kModeAcF2Slot * kModeAcSlotUs;
inline constexpr std::uint16_t kModeAcSpiBit = 0x0080;

// Code-word bit carried by each slot (0: framing, X, empty or SPI-less slot).
inline constexpr std::uint16_t kModeAcSlotBit[kModeAcSlots] = {
    0x0000,  // F1
    0x0010,  // C1
    0x1000,  // A1
    0x0020,  // C2
    0x2000,  // A2
    0x0040,  // C4
    0x4000,  // A4
    0x0000,  // X
    0x0100,  // B1
    0x0001,  // D1
    0x0200,  // B2
    0x0002,  // D2
    0x0400,  // B4
    0x0004,  // D4
    0x0000,  // F2
    0x0000,  // -
    0x0000,  // -
    0x0080,  // SPI
    0x0000,  // -
    0x0000,  // -
};

// Bits a code word may have set (12 code bits and SPI).
inline constexpr std::uint16_t kModeAcCodeMask = 0x7777 | kModeAcSpiBit;

// Pulse mask of the reply for a code word: bit k set = a pulse in slot k.
// Bits outside kModeAcCodeMask are ignored.
[[nodiscard]] constexpr std::uint32_t mode_ac_pulses(std::uint16_t code) noexcept {
  std::uint32_t p = (1u << 0) | (1u << kModeAcF2Slot);
  for (int k = 0; k < kModeAcSlots; ++k) {
    if (kModeAcSlotBit[k] != 0 && (code & kModeAcSlotBit[k]) != 0) p |= 1u << k;
  }
  return p;
}

// Code word of a pulse mask (inverse of mode_ac_pulses for the code slots;
// framing, X and the empty slots are not looked at).
[[nodiscard]] constexpr std::uint16_t mode_ac_code(std::uint32_t pulses) noexcept {
  std::uint16_t c = 0;
  for (int k = 0; k < kModeAcSlots; ++k) {
    if (kModeAcSlotBit[k] != 0 && (pulses >> k & 1u) != 0) c = static_cast<std::uint16_t>(c | kModeAcSlotBit[k]);
  }
  return c;
}

// Slots that must be empty in every reply: X and the four after F2 other
// than SPI (readsb checks the same ones).
inline constexpr std::uint32_t kModeAcEmptySlots =
    (1u << kModeAcXSlot) | (1u << 15) | (1u << 16) | (1u << 18) | (1u << 19);

// Duration from F1's leading edge to the end of the last pulse.
[[nodiscard]] constexpr double mode_ac_duration_us(std::uint16_t code) noexcept {
  return ((code & kModeAcSpiBit) ? kModeAcSpiSlot : kModeAcF2Slot) * kModeAcSlotUs + kModeAcPulseUs;
}

}  // namespace adsb::modes
