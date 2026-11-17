#pragma once

// `.adsbrec` — recorded Mode S frames with receive timestamps
// (README_UPDATES.md §11.1). One file per capture, from any source: a live
// readsb Beast feed, the synthetic generator, or imported historical data.
//
// Layout (all integers little-endian):
//
//   Header, 64 bytes
//     @0  char[8]  magic "ADSBREC1"
//     @8  u16      format version (1)
//     @10 u16      header size (64)
//     @12 u32      radio clock Hz (12'000'000 for Beast timestamps; 0 = none)
//     @16 u64      start wall-clock time, ns since the Unix epoch (labelling only)
//     @24 i32      receiver latitude, 1e-7 deg   (valid if flags bit 0)
//     @28 i32      receiver longitude, 1e-7 deg
//     @32 i32      receiver altitude, metres
//     @36 u32      receiver id
//     @40 char[16] source, NUL-padded ("readsb", "synthetic", "opensky", ...)
//     @56 u32      flags: bit 0 = receiver position valid
//     @60 u32      reserved (0)
//
//   Records, 20 + len bytes each, in non-decreasing rx_ns order
//     @0  u64  rx_ns      receive time, ns, monotonic clock domain (event time)
//     @8  u64  radio_ts   radio timestamp (Beast 12 MHz counter / sample index)
//     @16 u8   line       receiver line id (0 = A, 1 = B, ...)
//     @17 u8   type       Beast type byte: '1' Mode A/C, '2' short, '3' long
//     @18 u8   signal     signal level byte
//     @19 u8   len        payload length: 2, 7 or 14 (must match type)
//     @20 u8[len] payload
//
//   Footer, 24 bytes
//     @0  char[8] magic "ADSBEND1"
//     @8  u64     record count
//     @16 u32     CRC-32 of all record bytes
//     @20 u32     reserved (0)
//
// A file without a valid footer (the recorder was killed) can still be read
// with `allow_truncated`, which recovers every complete record.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

namespace adsb::record {

inline constexpr std::size_t kHeaderSize = 64;
inline constexpr std::size_t kRecordFixedSize = 20;
inline constexpr std::size_t kFooterSize = 24;
inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::uint32_t kBeastClockHz = 12'000'000;

struct RecordingInfo {
  std::uint32_t radio_clock_hz = kBeastClockHz;
  std::uint64_t start_wall_ns = 0;
  bool has_receiver_position = false;
  double receiver_lat = 0.0;
  double receiver_lon = 0.0;
  std::int32_t receiver_alt_m = 0;
  std::uint32_t receiver_id = 0;
  std::string source;  // at most 16 characters are stored
};

struct Record {
  std::uint64_t rx_ns = 0;
  std::uint64_t radio_ts = 0;
  std::uint8_t line = 0;
  std::uint8_t type = '3';
  std::uint8_t signal = 0;
  std::uint8_t len = 0;
  std::uint8_t payload[14] = {};

  [[nodiscard]] std::span<const std::uint8_t> frame() const noexcept { return {payload, len}; }
};

// Payload length a Beast type byte requires, or 0 if not a frame type.
[[nodiscard]] constexpr std::uint8_t payload_len_for_type(std::uint8_t type) noexcept {
  return type == '1' ? 2 : type == '2' ? 7 : type == '3' ? 14 : 0;
}

// Buffered, append-only writer. Not for the hot path directly: the pipeline
// feeds it through an SPSC ring from a dedicated thread.
class RecordingWriter {
 public:
  RecordingWriter() = default;
  RecordingWriter(RecordingWriter&& other) noexcept;
  RecordingWriter& operator=(RecordingWriter&& other) noexcept;
  RecordingWriter(const RecordingWriter&) = delete;
  RecordingWriter& operator=(const RecordingWriter&) = delete;
  ~RecordingWriter();

  // Creates (truncates) `path`. On failure returns false and sets `error`.
  [[nodiscard]] bool open(const std::string& path, const RecordingInfo& info, std::string* error);

  // Appends one record. Rejects records whose length does not match their
  // type, or whose rx_ns is earlier than the previous record's.
  [[nodiscard]] bool append(const Record& r);

  // Writes the footer and closes. Safe to call more than once.
  [[nodiscard]] bool close();

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

 private:
  std::FILE* f_ = nullptr;
  std::uint64_t count_ = 0;
  std::uint32_t crc_ = 0;
  std::uint64_t last_rx_ns_ = 0;
  bool failed_ = false;
};

// Reads a whole recording through a read-only memory map.
class RecordingReader {
 public:
  RecordingReader() = default;
  RecordingReader(RecordingReader&& other) noexcept;
  RecordingReader& operator=(RecordingReader&& other) noexcept;
  RecordingReader(const RecordingReader&) = delete;
  RecordingReader& operator=(const RecordingReader&) = delete;
  ~RecordingReader();

  // Opens and fully validates the file (header, every record, count, CRC).
  // With allow_truncated, a missing or bad footer is tolerated and the valid
  // prefix of records is used (truncated() then reports true).
  [[nodiscard]] bool open(const std::string& path, std::string* error,
                          bool allow_truncated = false);

  [[nodiscard]] const RecordingInfo& info() const noexcept { return info_; }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  // Sequential iteration. Returns false at the end.
  [[nodiscard]] bool next(Record& out) noexcept;
  void rewind() noexcept { pos_ = kHeaderSize; read_ = 0; }

 private:
  void unmap() noexcept;

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t records_end_ = 0;
  std::size_t pos_ = kHeaderSize;
  std::uint64_t read_ = 0;
  std::uint64_t count_ = 0;
  bool truncated_ = false;
  RecordingInfo info_{};
};

}  // namespace adsb::record
