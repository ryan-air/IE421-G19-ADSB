#include "adsb/record/recording.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "adsb/common/rng.hpp"

namespace {

using namespace adsb::record;
namespace fs = std::filesystem;

class TempFile {
 public:
  explicit TempFile(const char* name)
      : path_((fs::temp_directory_path() / (std::string("adsb_test_") + name + "_" +
                                            std::to_string(::getpid()) + ".adsbrec"))
                  .string()) {}
  ~TempFile() { std::error_code ec; fs::remove(path_, ec); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::vector<Record> sample_records(int n, std::uint64_t seed = 1) {
  adsb::Rng rng(seed);
  std::vector<Record> v;
  std::uint64_t t = 5'000'000'000;
  for (int i = 0; i < n; ++i) {
    Record r;
    t += rng.below(2'000'000);  // equal timestamps allowed
    r.rx_ns = t;
    r.radio_ts = rng.next() & 0xFFFFFFFFFFFFULL;
    r.line = static_cast<std::uint8_t>(rng.below(2));
    static constexpr std::uint8_t kTypes[] = {'1', '2', '3'};
    r.type = kTypes[rng.below(3)];
    r.len = payload_len_for_type(r.type);
    r.signal = static_cast<std::uint8_t>(rng.next());
    for (int k = 0; k < r.len; ++k) r.payload[k] = static_cast<std::uint8_t>(rng.next());
    v.push_back(r);
  }
  return v;
}

RecordingInfo sample_info() {
  RecordingInfo info;
  info.has_receiver_position = true;
  info.receiver_lat = 40.0392;
  info.receiver_lon = -88.2781;
  info.receiver_alt_m = 230;
  info.receiver_id = 7;
  info.start_wall_ns = 1'763'337'600'000'000'000ULL;
  info.source = "unit-test";
  return info;
}

void write_all(const std::string& path, const std::vector<Record>& recs) {
  RecordingWriter w;
  std::string err;
  ASSERT_TRUE(w.open(path, sample_info(), &err)) << err;
  for (const auto& r : recs) ASSERT_TRUE(w.append(r));
  ASSERT_TRUE(w.close());
  EXPECT_EQ(w.count(), recs.size());
}

bool same(const Record& a, const Record& b) {
  return a.rx_ns == b.rx_ns && a.radio_ts == b.radio_ts && a.line == b.line && a.type == b.type &&
         a.signal == b.signal && a.len == b.len && std::memcmp(a.payload, b.payload, a.len) == 0;
}

TEST(Recording, RoundTrip) {
  TempFile tf("roundtrip");
  const auto recs = sample_records(5000);
  write_all(tf.path(), recs);

  RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(tf.path(), &err)) << err;
  EXPECT_FALSE(rd.truncated());
  EXPECT_EQ(rd.count(), recs.size());
  EXPECT_EQ(rd.info().source, "unit-test");
  EXPECT_EQ(rd.info().receiver_id, 7u);
  EXPECT_NEAR(rd.info().receiver_lat, 40.0392, 1e-7);
  EXPECT_NEAR(rd.info().receiver_lon, -88.2781, 1e-7);
  EXPECT_EQ(rd.info().radio_clock_hz, kBeastClockHz);

  Record r;
  std::size_t i = 0;
  while (rd.next(r)) {
    ASSERT_LT(i, recs.size());
    ASSERT_TRUE(same(r, recs[i])) << i;
    ++i;
  }
  EXPECT_EQ(i, recs.size());
  rd.rewind();
  ASSERT_TRUE(rd.next(r));
  EXPECT_TRUE(same(r, recs[0]));
}

TEST(Recording, EmptyRecording) {
  TempFile tf("empty");
  write_all(tf.path(), {});
  RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(tf.path(), &err)) << err;
  EXPECT_EQ(rd.count(), 0u);
  Record r;
  EXPECT_FALSE(rd.next(r));
}

TEST(Recording, WriterRejectsBadRecords) {
  TempFile tf("reject");
  RecordingWriter w;
  std::string err;
  ASSERT_TRUE(w.open(tf.path(), sample_info(), &err));
  Record r;
  r.type = '3';
  r.len = 7;  // wrong length for a long frame
  EXPECT_FALSE(w.append(r));
  r.len = 14;
  r.rx_ns = 100;
  EXPECT_TRUE(w.append(r));
  r.rx_ns = 99;  // time going backwards
  EXPECT_FALSE(w.append(r));
  r.rx_ns = 100;  // equal is fine
  EXPECT_TRUE(w.append(r));
  EXPECT_TRUE(w.close());
}

std::vector<char> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void spit(const std::string& p, const std::vector<char>& d) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(d.data(), static_cast<std::streamsize>(d.size()));
}

TEST(Recording, DetectsCorruption) {
  TempFile tf("corrupt");
  const auto recs = sample_records(200);
  write_all(tf.path(), recs);
  auto bytes = slurp(tf.path());
  bytes[kHeaderSize + 25] = static_cast<char>(bytes[kHeaderSize + 25] ^ 0x01);  // inside record 0's payload
  spit(tf.path(), bytes);
  RecordingReader rd;
  std::string err;
  EXPECT_FALSE(rd.open(tf.path(), &err));
  EXPECT_NE(err.find("CRC"), std::string::npos) << err;
}

TEST(Recording, RecoversTruncatedFile) {
  TempFile tf("truncated");
  const auto recs = sample_records(300);
  write_all(tf.path(), recs);
  auto bytes = slurp(tf.path());
  // Simulate a recorder killed mid-write: drop the footer and half a record.
  bytes.resize(bytes.size() - kFooterSize - 9);
  spit(tf.path(), bytes);

  RecordingReader strict;
  std::string err;
  EXPECT_FALSE(strict.open(tf.path(), &err)) << "a file without a footer must not open silently";

  RecordingReader rd;
  ASSERT_TRUE(rd.open(tf.path(), &err, /*allow_truncated=*/true)) << err;
  EXPECT_TRUE(rd.truncated());
  EXPECT_EQ(rd.count(), recs.size() - 1) << "every complete record is recovered";
  Record r;
  std::size_t i = 0;
  while (rd.next(r)) ASSERT_TRUE(same(r, recs[i++]));
}

TEST(Recording, FailedOpenLeavesReaderEmpty) {
  // Regression: a failed open() after a successful one kept the previous
  // file's bounds, so next() read past the new (smaller) mapping.
  TempFile good("good"), bad("bad");
  write_all(good.path(), sample_records(2000));
  spit(bad.path(), std::vector<char>(64, 0));
  RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(good.path(), &err)) << err;
  EXPECT_FALSE(rd.open(bad.path(), &err));
  EXPECT_EQ(rd.count(), 0u);
  Record r;
  EXPECT_FALSE(rd.next(r));
  // Same after a corrupt-footer failure.
  ASSERT_TRUE(rd.open(good.path(), &err)) << err;
  auto bytes = slurp(good.path());
  bytes[kHeaderSize + 30] = static_cast<char>(bytes[kHeaderSize + 30] ^ 1);
  spit(bad.path(), bytes);
  EXPECT_FALSE(rd.open(bad.path(), &err));
  EXPECT_FALSE(rd.next(r));
}

TEST(Recording, RejectsNonRecordings) {
  TempFile tf("garbage");
  spit(tf.path(), std::vector<char>(200, 'x'));
  RecordingReader rd;
  std::string err;
  EXPECT_FALSE(rd.open(tf.path(), &err));
  EXPECT_FALSE(rd.open("/nonexistent/path.adsbrec", &err));
}

}  // namespace
