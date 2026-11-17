#include "adsb/record/recording.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <utility>

#include "adsb/common/hash.hpp"

namespace adsb::record {

static_assert(std::endian::native == std::endian::little, "recordings are little-endian");

namespace {

constexpr char kMagic[8] = {'A', 'D', 'S', 'B', 'R', 'E', 'C', '1'};
constexpr char kEndMagic[8] = {'A', 'D', 'S', 'B', 'E', 'N', 'D', '1'};

template <typename T>
void put(std::uint8_t* buf, std::size_t off, T v) noexcept {
  std::memcpy(buf + off, &v, sizeof(T));
}
template <typename T>
T get(const std::uint8_t* buf, std::size_t off) noexcept {
  T v;
  std::memcpy(&v, buf + off, sizeof(T));
  return v;
}

std::int32_t to_e7(double deg) noexcept {
  return static_cast<std::int32_t>(std::lround(deg * 1e7));
}

bool set_error(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

RecordingWriter::RecordingWriter(RecordingWriter&& o) noexcept
    : f_(std::exchange(o.f_, nullptr)), count_(o.count_), crc_(o.crc_),
      last_rx_ns_(o.last_rx_ns_), failed_(o.failed_) {}

RecordingWriter& RecordingWriter::operator=(RecordingWriter&& o) noexcept {
  if (this != &o) {
    (void)close();
    f_ = std::exchange(o.f_, nullptr);
    count_ = o.count_;
    crc_ = o.crc_;
    last_rx_ns_ = o.last_rx_ns_;
    failed_ = o.failed_;
  }
  return *this;
}

RecordingWriter::~RecordingWriter() { (void)close(); }

bool RecordingWriter::open(const std::string& path, const RecordingInfo& info,
                           std::string* error) {
  (void)close();
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return set_error(error, "cannot create " + path + ": " + std::strerror(errno));
  std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
  count_ = 0;
  crc_ = 0;
  last_rx_ns_ = 0;
  failed_ = false;

  std::uint8_t h[kHeaderSize] = {};
  std::memcpy(h, kMagic, 8);
  put<std::uint16_t>(h, 8, kFormatVersion);
  put<std::uint16_t>(h, 10, static_cast<std::uint16_t>(kHeaderSize));
  put<std::uint32_t>(h, 12, info.radio_clock_hz);
  put<std::uint64_t>(h, 16, info.start_wall_ns);
  put<std::int32_t>(h, 24, info.has_receiver_position ? to_e7(info.receiver_lat) : 0);
  put<std::int32_t>(h, 28, info.has_receiver_position ? to_e7(info.receiver_lon) : 0);
  put<std::int32_t>(h, 32, info.receiver_alt_m);
  put<std::uint32_t>(h, 36, info.receiver_id);
  std::memcpy(h + 40, info.source.data(), info.source.size() < 16 ? info.source.size() : 16);
  put<std::uint32_t>(h, 56, info.has_receiver_position ? 1u : 0u);
  if (std::fwrite(h, 1, kHeaderSize, f_) != kHeaderSize) {
    failed_ = true;
    return set_error(error, "write failed: " + path);
  }
  return true;
}

bool RecordingWriter::append(const Record& r) {
  if (!f_ || failed_) return false;
  if (r.len != payload_len_for_type(r.type)) return false;
  if (count_ != 0 && r.rx_ns < last_rx_ns_) return false;
  std::uint8_t buf[kRecordFixedSize + 14];
  put<std::uint64_t>(buf, 0, r.rx_ns);
  put<std::uint64_t>(buf, 8, r.radio_ts);
  buf[16] = r.line;
  buf[17] = r.type;
  buf[18] = r.signal;
  buf[19] = r.len;
  std::memcpy(buf + kRecordFixedSize, r.payload, r.len);
  const std::size_t n = kRecordFixedSize + r.len;
  if (std::fwrite(buf, 1, n, f_) != n) {
    failed_ = true;
    return false;
  }
  crc_ = crc32(buf, n, crc_);
  ++count_;
  last_rx_ns_ = r.rx_ns;
  return true;
}

bool RecordingWriter::close() {
  if (!f_) return !failed_;
  bool ok = !failed_;
  if (ok) {
    std::uint8_t ft[kFooterSize] = {};
    std::memcpy(ft, kEndMagic, 8);
    put<std::uint64_t>(ft, 8, count_);
    put<std::uint32_t>(ft, 16, crc_);
    ok = std::fwrite(ft, 1, kFooterSize, f_) == kFooterSize;
  }
  ok = (std::fclose(f_) == 0) && ok;
  f_ = nullptr;
  if (!ok) failed_ = true;
  return ok;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

RecordingReader::RecordingReader(RecordingReader&& o) noexcept
    : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)),
      records_end_(o.records_end_), pos_(o.pos_), read_(o.read_), count_(o.count_),
      truncated_(o.truncated_), info_(std::move(o.info_)) {}

RecordingReader& RecordingReader::operator=(RecordingReader&& o) noexcept {
  if (this != &o) {
    unmap();
    data_ = std::exchange(o.data_, nullptr);
    size_ = std::exchange(o.size_, 0);
    records_end_ = o.records_end_;
    pos_ = o.pos_;
    read_ = o.read_;
    count_ = o.count_;
    truncated_ = o.truncated_;
    info_ = std::move(o.info_);
  }
  return *this;
}

RecordingReader::~RecordingReader() { unmap(); }

void RecordingReader::unmap() noexcept {
  if (data_ != nullptr && size_ != 0) {
    munmap(const_cast<std::uint8_t*>(data_), size_);
  }
  data_ = nullptr;
  size_ = 0;
  // Reset iteration state too: after a failed open() the reader is empty
  // (next() returns false) instead of holding the previous file's bounds.
  records_end_ = 0;
  pos_ = kHeaderSize;
  read_ = 0;
  count_ = 0;
  truncated_ = false;
}

bool RecordingReader::open(const std::string& path, std::string* error, bool allow_truncated) {
  unmap();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return set_error(error, "cannot open " + path + ": " + std::strerror(errno));
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kHeaderSize)) {
    ::close(fd);
    return set_error(error, path + ": too small to be a recording");
  }
  size_ = static_cast<std::size_t>(st.st_size);
  void* m = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) {
    size_ = 0;
    return set_error(error, "mmap failed: " + path);
  }
#if defined(MADV_SEQUENTIAL)
  madvise(m, size_, MADV_SEQUENTIAL);
#endif
  data_ = static_cast<const std::uint8_t*>(m);
  auto fail = [&](const std::string& msg) {
    unmap();
    return set_error(error, msg);
  };

  // Header.
  if (std::memcmp(data_, kMagic, 8) != 0) return fail( path + ": not an .adsbrec file");
  if (get<std::uint16_t>(data_, 8) != kFormatVersion) {
    return fail( path + ": unsupported format version");
  }
  if (get<std::uint16_t>(data_, 10) != kHeaderSize) return fail( path + ": bad header size");
  info_ = RecordingInfo{};
  info_.radio_clock_hz = get<std::uint32_t>(data_, 12);
  info_.start_wall_ns = get<std::uint64_t>(data_, 16);
  const std::uint32_t flags = get<std::uint32_t>(data_, 56);
  info_.has_receiver_position = (flags & 1u) != 0;
  info_.receiver_lat = get<std::int32_t>(data_, 24) / 1e7;
  info_.receiver_lon = get<std::int32_t>(data_, 28) / 1e7;
  info_.receiver_alt_m = get<std::int32_t>(data_, 32);
  info_.receiver_id = get<std::uint32_t>(data_, 36);
  const char* src = reinterpret_cast<const char*>(data_ + 40);
  info_.source.assign(src, strnlen(src, 16));

  // Footer (if present and well-formed).
  bool footer_ok = false;
  std::uint64_t footer_count = 0;
  std::uint32_t footer_crc = 0;
  std::size_t body_end = size_;
  if (size_ >= kHeaderSize + kFooterSize &&
      std::memcmp(data_ + size_ - kFooterSize, kEndMagic, 8) == 0) {
    footer_ok = true;
    footer_count = get<std::uint64_t>(data_, size_ - kFooterSize + 8);
    footer_crc = get<std::uint32_t>(data_, size_ - kFooterSize + 16);
    body_end = size_ - kFooterSize;
  }
  if (!footer_ok && !allow_truncated) {
    return fail( path + ": missing footer (recording incomplete; use allow_truncated)");
  }

  // Walk every record: lengths, types, ordering, and the CRC.
  std::size_t pos = kHeaderSize;
  std::uint64_t n = 0;
  std::uint32_t crc = 0;
  std::uint64_t last_rx = 0;
  while (pos < body_end) {
    if (body_end - pos < kRecordFixedSize) break;
    const std::uint8_t type = data_[pos + 17];
    const std::uint8_t len = data_[pos + 19];
    const std::uint64_t rx = get<std::uint64_t>(data_, pos);
    if (len == 0 || len != payload_len_for_type(type) || body_end - pos < kRecordFixedSize + len ||
        (n != 0 && rx < last_rx)) {
      break;
    }
    crc = crc32(data_ + pos, kRecordFixedSize + len, crc);
    last_rx = rx;
    pos += kRecordFixedSize + len;
    ++n;
  }

  if (footer_ok && pos == body_end && n == footer_count && crc == footer_crc) {
    truncated_ = false;
  } else if (allow_truncated) {
    truncated_ = true;
  } else if (pos != body_end) {
    return fail( path + ": corrupt record at byte " + std::to_string(pos));
  } else if (n != footer_count) {
    return fail( path + ": record count mismatch");
  } else {
    return fail( path + ": CRC mismatch");
  }
  records_end_ = pos;
  count_ = n;
  rewind();
  return true;
}

bool RecordingReader::next(Record& out) noexcept {
  if (pos_ >= records_end_ || read_ >= count_) return false;
  out.rx_ns = get<std::uint64_t>(data_, pos_);
  out.radio_ts = get<std::uint64_t>(data_, pos_ + 8);
  out.line = data_[pos_ + 16];
  out.type = data_[pos_ + 17];
  out.signal = data_[pos_ + 18];
  out.len = data_[pos_ + 19];
  std::memcpy(out.payload, data_ + pos_ + kRecordFixedSize, out.len);
  if (out.len < sizeof(out.payload)) std::memset(out.payload + out.len, 0, sizeof(out.payload) - out.len);
  pos_ += kRecordFixedSize + out.len;
  ++read_;
  return true;
}

}  // namespace adsb::record
