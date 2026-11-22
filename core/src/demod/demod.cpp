#include "adsb/demod/demod.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "adsb/modes/crc24.hpp"

namespace adsb::demod {

std::size_t bytes_per_sample(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::kCu8:
    case SampleFormat::kCs8: return 2;
    case SampleFormat::kCs16: return 4;
    case SampleFormat::kCf32: return 8;
  }
  return 2;
}

const char* format_name(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::kCu8: return "cu8";
    case SampleFormat::kCs8: return "cs8";
    case SampleFormat::kCs16: return "cs16";
    case SampleFormat::kCf32: return "cf32";
  }
  return "?";
}

bool parse_format(const std::string& s, SampleFormat& out) noexcept {
  if (s == "cu8" || s == "uc8") { out = SampleFormat::kCu8; return true; }
  if (s == "cs8" || s == "ci8" || s == "sc8") { out = SampleFormat::kCs8; return true; }
  if (s == "cs16" || s == "ci16_le" || s == "sc16") { out = SampleFormat::kCs16; return true; }
  if (s == "cf32" || s == "cf32_le" || s == "fc32") { out = SampleFormat::kCf32; return true; }
  return false;
}

const char* magnitude_name(Magnitude m) noexcept {
  switch (m) {
    case Magnitude::kExact: return "exact";
    case Magnitude::kLut: return "lut";
    case Magnitude::kAlphaMaxBetaMin: return "ambm";
  }
  return "?";
}

namespace {

// 64K-entry table for cu8: index = I byte << 8 | Q byte, value =
// magnitude * 65535 / sqrt(2) (the largest magnitude, both at 127.5, is
// sqrt(2) relative to full scale).
const std::uint16_t* cu8_table() {
  static const std::vector<std::uint16_t> table = [] {
    std::vector<std::uint16_t> t(65536);
    for (int i = 0; i < 256; ++i) {
      for (int q = 0; q < 256; ++q) {
        const double fi = (i - 127.5) / 127.5;
        const double fq = (q - 127.5) / 127.5;
        const double m = std::sqrt(fi * fi + fq * fq) / std::sqrt(2.0);
        t[static_cast<std::size_t>(i << 8 | q)] = static_cast<std::uint16_t>(std::lround(m * 65535.0));
      }
    }
    return t;
  }();
  return table.data();
}

inline float ambm(float i, float q) noexcept {
  const float a = std::fabs(i);
  const float b = std::fabs(q);
  const float mx = a > b ? a : b;
  const float mn = a > b ? b : a;
  // The equiripple pair: error between -3.96% (on an axis) and +3.96% (at
  // min/max = 0.414), so the mean level is unbiased.
  return 0.96043387f * mx + 0.39782473f * mn;
}

template <typename T, typename Conv>
void mag_loop(const std::uint8_t* data, std::size_t n, float scale, Magnitude m, Conv conv, float* out) noexcept {
  const T* s = reinterpret_cast<const T*>(data);
  if (m == Magnitude::kAlphaMaxBetaMin) {
    for (std::size_t k = 0; k < n; ++k) out[k] = ambm(conv(s[2 * k]), conv(s[2 * k + 1])) * scale;
  } else {
    for (std::size_t k = 0; k < n; ++k) {
      const float i = conv(s[2 * k]);
      const float q = conv(s[2 * k + 1]);
      out[k] = std::sqrt(i * i + q * q) * scale;
    }
  }
}

}  // namespace

void magnitudes(const std::uint8_t* data, std::size_t n, SampleFormat f, Magnitude m, float* out) noexcept {
  switch (f) {
    case SampleFormat::kCu8:
      if (m == Magnitude::kLut) {
        const std::uint16_t* t = cu8_table();
        constexpr float kScale = 1.41421356237f / 65535.0f;
        for (std::size_t k = 0; k < n; ++k) {
          out[k] = static_cast<float>(t[static_cast<std::size_t>(data[2 * k]) << 8 | data[2 * k + 1]]) * kScale;
        }
      } else {
        mag_loop<std::uint8_t>(data, n, 1.0f / 127.5f, m,
                               [](std::uint8_t v) { return static_cast<float>(v) - 127.5f; }, out);
      }
      return;
    case SampleFormat::kCs8:
      mag_loop<std::int8_t>(data, n, 1.0f / 128.0f, m, [](std::int8_t v) { return static_cast<float>(v); }, out);
      return;
    case SampleFormat::kCs16: {
      // Little-endian 16-bit: assemble from bytes (no alignment assumption).
      for (std::size_t k = 0; k < n; ++k) {
        const auto* p = data + 4 * k;
        const auto i = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | p[1] << 8));
        const auto q = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[2] | p[3] << 8));
        const float fi = static_cast<float>(i);
        const float fq = static_cast<float>(q);
        out[k] = (m == Magnitude::kAlphaMaxBetaMin ? ambm(fi, fq) : std::sqrt(fi * fi + fq * fq)) * (1.0f / 32768.0f);
      }
      return;
    }
    case SampleFormat::kCf32:
      for (std::size_t k = 0; k < n; ++k) {
        float iq[2];
        std::memcpy(iq, data + 8 * k, 8);
        const float v = m == Magnitude::kAlphaMaxBetaMin ? ambm(iq[0], iq[1]) : std::sqrt(iq[0] * iq[0] + iq[1] * iq[1]);
        out[k] = std::isfinite(v) ? v : 0.0f;  // NaN / inf samples count as silence
      }
      return;
  }
}

bool validate(const DemodConfig& c, std::string* why) {
  auto fail = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  if (!(c.sample_rate_hz >= 1.9e6 && c.sample_rate_hz <= 100e6)) return fail("sample rate must be in [1.9, 100] MS/s");
  if (c.phases < 1 || c.phases > 16) return fail("phases must be in [1, 16]");
  if (!(c.min_signal_ratio >= 1.0 && c.min_signal_ratio <= 1000.0)) return fail("min_signal_ratio must be in [1, 1000]");
  if (!(c.min_pulse_quiet >= 1.0 && c.min_pulse_quiet <= 1000.0)) return fail("min_pulse_quiet must be in [1, 1000]");
  if (!(c.gate_ratio >= 0.0 && c.gate_ratio <= 1000.0)) return fail("gate_ratio must be in [0, 1000]");
  if (!(c.resume_overlap_us >= 0.0 && c.resume_overlap_us <= 56.0)) return fail("resume_overlap_us must be in [0, 56]");
  if (!(c.address_ttl_s > 0.0 && c.address_ttl_s <= 1e6)) return fail("address_ttl_s must be in (0, 1e6]");
  if (c.block_samples < 1024 || c.block_samples > (std::size_t{1} << 26)) return fail("block_samples must be in [1024, 2^26]");
  return true;
}

namespace {

constexpr int kPreambleChips = 16;             // 8 us of 0.5 us chips
constexpr double kClimbStepUs = 0.04;          // step-5 hill-climb step
constexpr int kClimbSteps = 13;                // ... and its reach, in steps either way
constexpr int kMaxChips = kPreambleChips + 2 * 112;
constexpr int kPulseChips[4] = {0, 2, 7, 9};
constexpr int kQuietChips[12] = {1, 3, 4, 5, 6, 8, 10, 11, 12, 13, 14, 15};

// Address table for Address/Parity frames: open addressing, linear probing,
// entries expire after ttl samples (replaced in place when found stale).
class AddressTable {
 public:
  static constexpr std::size_t kSize = 4096;  // power of two
  static constexpr std::size_t kProbe = 32;

  void add(std::uint32_t icao, std::uint64_t now) noexcept {
    std::size_t h = hash(icao);
    std::size_t victim = h;
    std::uint64_t oldest = ~std::uint64_t{0};
    for (std::size_t i = 0; i < kProbe; ++i) {
      Entry& e = t_[(h + i) & (kSize - 1)];
      if (e.used && e.icao == icao) {
        e.seen = now;
        return;
      }
      if (!e.used) {
        e = {icao, now, true};
        return;
      }
      if (e.seen < oldest) {
        oldest = e.seen;
        victim = (h + i) & (kSize - 1);
      }
    }
    t_[victim] = {icao, now, true};
  }

  [[nodiscard]] bool contains(std::uint32_t icao, std::uint64_t now, std::uint64_t ttl) const noexcept {
    const std::size_t h = hash(icao);
    for (std::size_t i = 0; i < kProbe; ++i) {
      const Entry& e = t_[(h + i) & (kSize - 1)];
      if (!e.used) return false;
      if (e.icao == icao) return now - e.seen <= ttl;
    }
    return false;
  }

 private:
  struct Entry {
    std::uint32_t icao = 0;
    std::uint64_t seen = 0;
    bool used = false;
  };
  static std::size_t hash(std::uint32_t icao) noexcept {
    return static_cast<std::size_t>((icao * 0x9E3779B1u) >> 20) & (kSize - 1);
  }
  std::array<Entry, kSize> t_{};
};

inline bool address_parity_df(unsigned df) noexcept {
  return df == 0 || df == 4 || df == 5 || df == 16 || df == 20 || df == 21;
}

inline bool supported_df(unsigned df) noexcept {
  return address_parity_df(df) || df == 11 || df == 17 || df == 18;
}

}  // namespace

struct Demodulator::Impl {
  // Sparse weights of every chip for every phase: chip c of phase p is
  // sum_j w[off + j] * m[t + first + j].
  struct Chip {
    std::int32_t first = 0;
    std::uint32_t off = 0;
    std::uint32_t count = 0;
  };
  int phases = 5;
  std::vector<Chip> chips;      // [phase * kMaxChips + chip]
  std::vector<float> weights;
  float chip_len = 1.2f;        // samples per chip
  // Step-1 gate: window sums of `win` samples starting at t + gate_k[p] or
  // one later hold most of preamble pulse p for any start in [t, t + 1).
  std::ptrdiff_t win = 2;
  std::ptrdiff_t gate_k[4] = {};
  float gate_scale = 0.0f;      // threshold = gate_scale * noise
  std::vector<float> wsum;      // window sums, per buffered position
  std::vector<std::uint8_t> gate;  // step-1 result, per position from t0
  std::ptrdiff_t need = 0;      // samples after t a long message can reach
  std::ptrdiff_t back = 4;      // samples kept before the next position (covers the step-5 climb)
  std::uint32_t syn56[56] = {}, syn112[112] = {};
  AddressTable addresses;
  std::vector<double> templ;    // step-5 expected-magnitude template

  // Sequence detector (see detect_sequence). For every phase, the samples
  // of a message in order: each overlaps chip c_lo by ov_lo and chip
  // c_lo + 1 by ov_hi (a sample never spans more than two chips, as a chip
  // is at least one sample long); bit_begin[i] is the first sample whose
  // last chip belongs to data bit i (or later).
  struct SampleOv {
    std::int16_t c_lo = 0;
    float ov_lo = 0.0f, ov_hi = 0.0f;
  };
  std::ptrdiff_t sov_k0 = 0;    // first sample, relative to t
  std::size_t sov_n = 0;        // samples per phase
  std::vector<SampleOv> sov;    // [phase * sov_n + j]
  std::vector<std::uint16_t> bit_begin;  // [phase * 113 + i]
  // For every sample (and phase), the role of its two chips within the bit
  // it belongs to: 1 = the previous bit's second half, 2 = this bit's first
  // half, 3 = its second half, 0 = neither (preamble, or past the frame).
  std::vector<std::uint8_t> sov_role;    // [(phase * sov_n + j) * 2 + {lo, hi}]
  std::vector<std::uint8_t> trace;       // traceback, [bit * 2 + state]

  // Maximum-likelihood bits for a frame of `bits` bits starting at phase p
  // of position t. Each sample's expected magnitude is sqrt((A x)^2 + mu^2),
  // x = its overlap with the chips that carry a pulse; A and mu come from a
  // least-squares fit over the preamble samples. A 2-state Viterbi (state =
  // previous bit) finds the bits with the least squared error, so a sample
  // shared by two half-bits counts for what it is instead of being split
  // between both. Returns false if the preamble fit fails.
  // Runs the detector over `bits` bits (56 or 112) and writes the most
  // likely frame of that length to `bytes`; with bits = 112 it also writes
  // the most likely 56-bit frame to `bytes56` (the path through the first
  // 56 bits does not depend on the bits after them, so one pass serves
  // both lengths). Returns false if the preamble fit fails.
  bool detect_sequence(const float* mt, int p, int bits, std::uint8_t* bytes, std::uint8_t* bytes56) {
    const SampleOv* so = sov.data() + static_cast<std::size_t>(p) * sov_n;
    const std::uint8_t* role = sov_role.data() + static_cast<std::size_t>(p) * sov_n * 2;
    const std::uint16_t* bb = bit_begin.data() + static_cast<std::size_t>(p) * 113;
    // Preamble fit: m = A x + mu.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    auto pre_on = [](int c) { return c == 0 || c == 2 || c == 7 || c == 9; };
    for (std::size_t j = 0; j < bb[0]; ++j) {
      if (so[j].c_lo < 0) continue;
      const int c = so[j].c_lo;
      const double x = (pre_on(c) ? static_cast<double>(so[j].ov_lo) : 0.0) + (pre_on(c + 1) ? static_cast<double>(so[j].ov_hi) : 0.0);
      const auto y = static_cast<double>(mt[sov_k0 + static_cast<std::ptrdiff_t>(j)]);
      sx += x; sy += y; sxx += x * x; sxy += x * y;
      ++n;
    }
    if (n < 4) return false;
    const double var = sxx - sx * sx / n;
    if (var <= 0) return false;
    const auto amp = static_cast<float>((sxy - sx * sy / n) / var);
    const auto mu = static_cast<float>(std::max(0.0, (sy - static_cast<double>(amp) * sx) / n));
    if (!(amp > 0.0f)) return false;
    const float a2 = amp * amp, mu2 = mu * mu;
    float cost[2] = {0.0f, 1e30f};  // before bit 0: "previous bit" 1 = last chip (15) empty
    float cost56[2] = {0.0f, 0.0f};
    for (int i = 0; i < bits; ++i) {
      // Squared error of every sample of this bit for each (previous bit,
      // this bit): a sample's chips are its bit's two half-bits or the
      // previous bit's second half-bit (role 1, 2, 3; 0 = no pulse there).
      // Path cost of each (previous bit, this bit), accumulated sample by
      // sample onto the previous path's cost, in float: exactly the sums
      // (and so the decisions) of a loop over the states.
      float c[2][2] = {{cost[0], cost[0]}, {cost[1], cost[1]}};
      for (std::size_t j = bb[i]; j < bb[i + 1]; ++j) {
        const float y = mt[sov_k0 + static_cast<std::ptrdiff_t>(j)];
        const float lo = so[j].ov_lo, hi = so[j].ov_hi;
        const std::uint8_t r_lo = role[2 * j], r_hi = role[2 * j + 1];
        // The four predictions (pulse in neither / the second / the first /
        // both of the sample's chips), each squared error computed once.
        auto err = [&](float x) {
          const float d = y - std::sqrt(a2 * x * x + mu2);
          return d * d;
        };
        const float e[2][2] = {{err(lo * 0.0f + hi * 0.0f), err(lo * 0.0f + hi * 1.0f)},
                                {err(lo * 1.0f + hi * 0.0f), err(lo * 1.0f + hi * 1.0f)}};
        for (int prev = 0; prev < 2; ++prev) {
          for (int b = 0; b < 2; ++b) {
            auto on = [&](std::uint8_t r) -> int {
              switch (r) {
                case 1: return i == 0 ? 0 : 1 - prev;
                case 2: return b;
                case 3: return 1 - b;
                default: return 0;
              }
            };
            c[prev][b] += e[on(r_lo)][on(r_hi)];
          }
        }
      }
      float next[2];
      for (int b = 0; b < 2; ++b) {
        const float via0 = cost[0] < 1e30f ? c[0][b] : 1e30f;
        const float via1 = cost[1] < 1e30f ? c[1][b] : 1e30f;
        // Ties go to previous bit 0, as the earlier per-state loop did.
        const bool one = via1 < via0;
        next[b] = one ? via1 : via0;
        trace[static_cast<std::size_t>(2 * i + b)] = static_cast<std::uint8_t>(one ? 1 : 0);
      }
      cost[0] = next[0];
      cost[1] = next[1];
      if (i == 55) {
        cost56[0] = cost[0];
        cost56[1] = cost[1];
      }
    }
    auto traceback = [&](int nb, const float* end_cost, std::uint8_t* out) {
      int state = end_cost[1] < end_cost[0] ? 1 : 0;
      std::memset(out, 0, 14);
      for (int i = nb - 1; i >= 0; --i) {
        if (state) out[i / 8] = static_cast<std::uint8_t>(out[i / 8] | (0x80u >> (i % 8)));
        state = trace[static_cast<std::size_t>(2 * i + state)];
      }
    };
    traceback(bits, cost, bytes);
    if (bits == 112 && bytes56) traceback(56, cost56, bytes56);
    return true;
  }
  std::uint64_t ttl = 0;

  // Up to 4 samples per chip (rates up to 6 MS/s): every chip has 4
  // weights, zero-padded, so a chip is 4 multiply-adds without a loop.
  bool fixed4 = true;
  std::vector<float> w4;            // [(phase * kMaxChips + chip) * 4]
  std::vector<std::int32_t> first4;  // [phase * kMaxChips + chip]

  [[nodiscard]] float chip(const float* m, std::ptrdiff_t t, int phase, int c) const noexcept {
    const auto idx = static_cast<std::size_t>(phase * kMaxChips + c);
    if (fixed4) {
      const float* w = w4.data() + 4 * idx;
      const float* x = m + t + first4[idx];
      return w[0] * x[0] + w[1] * x[1] + w[2] * x[2] + w[3] * x[3];
    }
    const Chip& ch = chips[idx];
    const float* w = weights.data() + ch.off;
    const float* x = m + t + ch.first;
    float s = 0.0f;
    for (std::uint32_t j = 0; j < ch.count; ++j) s += w[j] * x[j];
    return s;
  }
};

Demodulator::Demodulator(const DemodConfig& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>()) {
  spu_ = cfg_.sample_rate_hz / 1e6;
  Impl& im = *impl_;
  im.phases = std::clamp(cfg_.phases, 1, 16);
  im.chip_len = static_cast<float>(spu_ / 2);
  im.chips.resize(static_cast<std::size_t>(im.phases * kMaxChips));
  for (int p = 0; p < im.phases; ++p) {
    const double frac = static_cast<double>(p) / im.phases;
    for (int c = 0; c < kMaxChips; ++c) {
      const double a = frac + c * spu_ / 2 + 0.5;  // shifted: sample k covers [k, k + 1)
      const double b = a + spu_ / 2;
      const auto ka = static_cast<std::int32_t>(std::floor(a));
      const auto kb = static_cast<std::int32_t>(std::floor(b));
      Impl::Chip& ch = im.chips[static_cast<std::size_t>(p * kMaxChips + c)];
      ch.first = ka;
      ch.off = static_cast<std::uint32_t>(im.weights.size());
      for (std::int32_t k = ka; k <= kb; ++k) {
        const double lo = std::max(a, static_cast<double>(k));
        const double hi = std::min(b, static_cast<double>(k + 1));
        if (hi > lo || k == ka) im.weights.push_back(static_cast<float>(std::max(0.0, hi - lo)));
        else break;
      }
      ch.count = static_cast<std::uint32_t>(im.weights.size() - ch.off);
    }
  }
  for (const Impl::Chip& ch : im.chips) im.fixed4 = im.fixed4 && ch.count <= 4;
  if (im.fixed4) {
    im.w4.assign(im.chips.size() * 4, 0.0f);
    im.first4.resize(im.chips.size());
    for (std::size_t i = 0; i < im.chips.size(); ++i) {
      im.first4[i] = im.chips[i].first;
      for (std::uint32_t j = 0; j < im.chips[i].count; ++j) im.w4[4 * i + j] = im.weights[im.chips[i].off + j];
    }
  }
  // Gate (see Impl): a window of floor(chip) + 1 samples starting at the
  // sample that holds a pulse's start covers at least floor(chip) samples of
  // it. Noise alone fills a window with `win` noise means; a pulse at the
  // weakest accepted level adds min_signal_ratio per covered sample.
  {
    const double c = spu_ / 2;
    const double covered = std::floor(c);
    im.win = static_cast<std::ptrdiff_t>(covered) + 1;
    for (int i = 0; i < 4; ++i) {
      im.gate_k[i] = static_cast<std::ptrdiff_t>(std::floor(kPulseChips[i] * c + 0.5));
    }
    im.gate_scale = static_cast<float>(cfg_.gate_ratio *
                                       (cfg_.min_signal_ratio * covered + static_cast<double>(im.win) - covered));
  }
  // The timing climb reaches (kClimbSteps + 1) steps either side of a
  // position, and the template a sample beyond that: the samples kept before
  // a position (back) and needed after its longest frame (need) cover it.
  const auto climb = static_cast<std::ptrdiff_t>(std::ceil((kClimbSteps + 1) * kClimbStepUs * spu_)) + 2;
  im.back = climb + 2;
  im.need = static_cast<std::ptrdiff_t>(std::ceil((kMaxChips * spu_) / 2 + 1.0)) + 6 + climb;
  // Single-bit error syndromes (CRC is linear: the syndrome of a frame with
  // one bit flipped is the syndrome of that bit alone).
  for (int bits : {56, 112}) {
    for (int i = 0; i < bits; ++i) {
      std::uint8_t f[14] = {};
      f[i / 8] = static_cast<std::uint8_t>(0x80u >> (i % 8));
      const std::uint32_t s = modes::crc24_syndrome(f, static_cast<std::size_t>(bits / 8));
      (bits == 56 ? im.syn56 : im.syn112)[i] = s;
    }
  }
  // Sequence-detector tables: samples from the one holding the start of
  // chip 0 to the one holding the end of the last chip, for every phase.
  {
    const double h = spu_ / 2;
    im.sov_k0 = 0;
    const auto k_end = static_cast<std::ptrdiff_t>(std::floor(1.0 + kMaxChips * h + 0.5));
    im.sov_n = static_cast<std::size_t>(k_end - im.sov_k0 + 1);
    im.sov.assign(static_cast<std::size_t>(im.phases) * im.sov_n, {});
    im.bit_begin.assign(static_cast<std::size_t>(im.phases) * 113, 0);
    im.trace.assign(2 * 112, 0);
    for (int p = 0; p < im.phases; ++p) {
      const double frac = static_cast<double>(p) / im.phases;
      Impl::SampleOv* so = im.sov.data() + static_cast<std::size_t>(p) * im.sov_n;
      std::uint16_t* bb = im.bit_begin.data() + static_cast<std::size_t>(p) * 113;
      int bit = 0;
      for (std::size_t j = 0; j < im.sov_n; ++j) {
        const double lo = static_cast<double>(im.sov_k0 + static_cast<std::ptrdiff_t>(j)) - 0.5 - frac;  // chip units * h
        const double hi = lo + 1.0;
        auto ov = [&](int c) { return std::max(0.0, std::min(hi, (c + 1) * h) - std::max(lo, c * h)); };
        int c_lo = static_cast<int>(std::floor(lo / h));
        if (c_lo < 0 && ov(0) > 0) c_lo = -1;
        Impl::SampleOv& e = so[j];
        if (hi <= 0.0 || c_lo >= kMaxChips) {
          e.c_lo = -1;  // outside the message
        } else {
          e.c_lo = static_cast<std::int16_t>(std::max(c_lo, 0));
          e.ov_lo = static_cast<float>(ov(e.c_lo));
          e.ov_hi = e.c_lo + 1 < kMaxChips ? static_cast<float>(ov(e.c_lo + 1)) : 0.0f;
          if (c_lo < 0) {  // the sample starts before chip 0: only chip 0 counts
            e.ov_lo = static_cast<float>(ov(0));
            e.ov_hi = static_cast<float>(ov(1));
          }
        }
        // The last chip this sample touches decides which bit it belongs to.
        const int c_hi = e.c_lo < 0 ? -1 : (e.ov_hi > 0.0f ? e.c_lo + 1 : e.c_lo);
        while (bit < 113 && c_hi >= kPreambleChips + 2 * bit) bb[bit++] = static_cast<std::uint16_t>(j);
      }
      while (bit < 113) bb[bit++] = static_cast<std::uint16_t>(im.sov_n);
    }
    im.sov_role.assign(static_cast<std::size_t>(im.phases) * im.sov_n * 2, 0);
    for (int p = 0; p < im.phases; ++p) {
      const Impl::SampleOv* so = im.sov.data() + static_cast<std::size_t>(p) * im.sov_n;
      const std::uint16_t* bb = im.bit_begin.data() + static_cast<std::size_t>(p) * 113;
      std::uint8_t* role = im.sov_role.data() + static_cast<std::size_t>(p) * im.sov_n * 2;
      for (int i = 0; i < 112; ++i) {
        const int first = kPreambleChips + 2 * i;
        auto r = [&](int chip) -> std::uint8_t {
          if (chip == first - 1 && i > 0) return 1;
          if (chip == first) return 2;
          if (chip == first + 1) return 3;
          return 0;
        };
        for (std::size_t j = bb[i]; j < bb[i + 1]; ++j) {
          role[2 * j] = r(so[j].c_lo);
          role[2 * j + 1] = r(so[j].c_lo + 1);
        }
      }
    }
  }
  im.ttl = static_cast<std::uint64_t>(cfg_.address_ttl_s * cfg_.sample_rate_hz);
  im.templ.assign(static_cast<std::size_t>(im.need + im.back) + 16, 0.0);

  // Everything is allocated here: room for a block, the held-back samples,
  // the up to 1023 samples that wait for the first noise block, and the
  // zero padding flush() puts after the data.
  const std::size_t data_cap = cfg_.block_samples + static_cast<std::size_t>(im.need + im.back) + kNoiseBlock + 64;
  buf_.assign(data_cap + static_cast<std::size_t>(im.need) + 64, 0.0f);
  im.wsum.assign(buf_.size(), 0.0f);
  im.gate.assign(buf_.size(), 0);
  // Frames are at least (64 - resume_overlap_us) us apart (a short frame,
  // minus the overlap scanning resumes with), and at least 8 us.
  const double min_frame_samples = std::max(8.0, 64.0 - cfg_.resume_overlap_us) * spu_;
  out_.resize(static_cast<std::size_t>(static_cast<double>(data_cap) / min_frame_samples) + 16);
  // Noise estimates: every block the buffer can hold at once, and the
  // history before the oldest of them.
  block_est_.assign(data_cap / kNoiseBlock + kNoiseHistory + 4, 0.0f);
  block_id_.assign(block_est_.size(), ~std::uint64_t{0});
}

Demodulator::~Demodulator() = default;

std::span<const DemodFrame> Demodulator::feed(const std::uint8_t* data, std::size_t n, SampleFormat f) {
  n = std::min(n, cfg_.block_samples);
  magnitudes(data, n, f, cfg_.magnitude, buf_.data() + valid_);
  append(n);
  scan(false);
  return {out_.data(), n_out_};
}

std::span<const DemodFrame> Demodulator::feed_magnitudes(const float* mag, std::size_t n) {
  n = std::min(n, cfg_.block_samples);
  std::memcpy(buf_.data() + valid_, mag, n * sizeof(float));
  append(n);
  scan(false);
  return {out_.data(), n_out_};
}

std::span<const DemodFrame> Demodulator::flush() {
  // Pad with zeros so the held-back positions can be scanned; frames that
  // would reach into the padding fail their checks or are discarded below.
  scan(true);
  return {out_.data(), n_out_};
}

// Noise floor. Each 1024-sample block is cut into 64 windows of 16 samples
// (6.7 us at 2.4 MS/s); the estimate is the mean of the lowest quarter of
// the window means. Averaging over a window smooths out quantization (with
// noise near one ADC step, cu8 magnitudes take only a few values, so a plain
// percentile jumps between them), and the lowest quarter stays on windows
// without replies unless the channel is busy more than about 3/4 of the
// time. A window mean of noise alone is close to Gaussian with a standard
// deviation of 0.523 / 4 of the mean (Rayleigh: sd / mean = 0.523); the
// mean of its lowest quarter is 1.271 such deviations below the mean, so the
// estimate is divided by 1 - 1.271 * 0.1308 = 0.8338. The floor used at a
// position is the smallest estimate of the 8 blocks before its block (block
// 0 uses itself), so it depends only on the samples, not on how they were
// fed in.
namespace {
constexpr float kNoiseQuarterScale = 1.0f / 0.8338f;

float lowest_quarter_mean(const float* win, std::size_t n) {
  std::array<float, 65> tmp{};  // up to 64 windows and a partial one
  std::copy(win, win + n, tmp.begin());
  const std::size_t q = std::max<std::size_t>(1, n / 4);
  // The q smallest, ascending (what a partial sort gives, for less work):
  // the sum below then adds the same values in the same order.
  std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(q - 1), tmp.begin() + static_cast<std::ptrdiff_t>(n));
  std::sort(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(q));
  float sum = 0.0f;
  for (std::size_t i = 0; i < q; ++i) sum += tmp[i];
  return sum / static_cast<float>(q) * kNoiseQuarterScale;
}
}  // namespace

void Demodulator::finish_noise_block() noexcept {
  const float est = n_sub_ > 0 ? lowest_quarter_mean(sub_.data(), n_sub_) : 0.0f;
  const std::uint64_t id = (pos_ - 1) / kNoiseBlock;  // the block that just ended (pos_ is past it)
  block_est_[id % block_est_.size()] = est;
  block_id_[id % block_id_.size()] = id;
  have_block_ = true;
  last_block_ = id;
  n_sub_ = 0;
  noise_ = est;
}

float Demodulator::noise_for(std::uint64_t block, bool final) const noexcept {
  // The 8 blocks before `block` (those still present: skip() leaves holes).
  const std::uint64_t lo = block > kNoiseHistory ? block - kNoiseHistory : 0;
  bool found = false;
  float best = 0.0f;
  for (std::uint64_t b = lo; b < block; ++b) {
    const std::size_t slot = b % block_est_.size();
    if (block_id_[slot] != b) continue;
    best = found ? std::min(best, block_est_[slot]) : block_est_[slot];
    found = true;
  }
  if (found) return best;
  // The start of the stream (or of the samples after a long gap): the block
  // itself once complete, else the latest earlier block, else (at the end of
  // a short stream) the partial block's own windows.
  if (const std::size_t slot = block % block_est_.size(); block_id_[slot] == block) return block_est_[slot];
  if (have_block_ && last_block_ < block) return block_est_[last_block_ % block_est_.size()];
  if (!final) return 0.0f;
  std::array<float, 65> w{};  // the block's windows and a partial one
  std::copy(sub_.begin(), sub_.begin() + static_cast<std::ptrdiff_t>(n_sub_), w.begin());
  std::size_t n = n_sub_;
  if (win_count_ > 0) {
    float sum = 0.0f;
    for (std::size_t i = 0; i < win_count_; ++i) sum += win_buf_[i];
    w[n++] = sum / static_cast<float>(win_count_);
  }
  return n > 0 ? lowest_quarter_mean(w.data(), n) : 0.0f;
}

std::span<const DemodFrame> Demodulator::skip(std::uint64_t n) {
  n_out_ = 0;
  if (n == 0) return {};  // no gap: keep the held samples (a flush would lose frames across them)
  scan(true);  // as at the end of a stream: the buffer is emptied (base_ == pos_)
  // The partial noise window and block straddle the gap: drop them.
  win_count_ = 0;
  n_sub_ = 0;
  pos_ += n;
  base_ = pos_;
  valid_ = 0;
  next_ = base_;
  stats_.skipped += n;
  return {out_.data(), n_out_};
}

namespace {
// Sum of 16 values, one after another from zero: the order the noise
// floor has always used, so its value (and every decision made from it) is
// unchanged by how the samples arrive.
inline float window_sum16(const float* x) noexcept {
  float sum = 0.0f;
  for (int j = 0; j < 16; ++j) sum += x[j];
  return sum;
}
}  // namespace

void Demodulator::append(std::size_t n) {
  static_assert(kNoiseWindow == 16 && kNoiseBlock % kNoiseWindow == 0);
  const float* m = buf_.data() + valid_;
  std::size_t k = 0;
  while (k < n) {
    const std::uint64_t in_block = pos_ % kNoiseBlock;
    if (win_count_ == 0 && k + kNoiseWindow <= n && in_block + kNoiseWindow <= kNoiseBlock) {
      // A whole window inside one block, straight from the buffer.
      sub_[n_sub_++] = window_sum16(m + k) / static_cast<float>(kNoiseWindow);
      pos_ += kNoiseWindow;
      k += kNoiseWindow;
      if (in_block + kNoiseWindow == kNoiseBlock) finish_noise_block();
      continue;
    }
    // A window split across feed() calls, or not aligned to the block (after
    // a skip): collect it sample by sample, then sum it the same way.
    win_buf_[win_count_++] = m[k++];
    ++pos_;
    if (win_count_ == kNoiseWindow) {
      sub_[n_sub_++] = window_sum16(win_buf_.data()) / static_cast<float>(kNoiseWindow);
      win_count_ = 0;
    }
    if ((pos_ - 1) % kNoiseBlock == kNoiseBlock - 1) finish_noise_block();
  }
  valid_ += n;
  stats_.samples += n;
}

void Demodulator::scan(bool final) {
  Impl& im = *impl_;
  n_out_ = 0;
  const float* m = buf_.data();
  const auto valid = static_cast<std::ptrdiff_t>(valid_);
  // Positions whose longest possible message fits in the buffer. At the end
  // of the stream every position is tried and a frame must fit itself.
  std::ptrdiff_t end = valid - im.need;
  std::vector<float>& buf = buf_;
  if (final) {
    // Zero padding so reads past the data stay in bounds.
    // (buf_ was allocated with this room.)
    const std::size_t pad = std::min(static_cast<std::size_t>(im.need) + 8, buf.size() - valid_);
    std::fill(buf.begin() + valid, buf.begin() + valid + static_cast<std::ptrdiff_t>(pad), 0.0f);
    m = buf.data();
    end = valid;
  }
  // Until the first noise block is complete nothing can be judged (at the
  // end of a short stream the partial block is used).
  if (!have_block_ && !final) end = 0;
  const auto pulse_quiet = static_cast<float>(cfg_.min_pulse_quiet);
  const float chip_len = im.chip_len;

  std::ptrdiff_t t = std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(next_ - base_), im.back);
  const bool fix = cfg_.fix_single_bit;

  // Step 1 for the whole range at once (branch-free loops, vectorized).
  const std::ptrdiff_t t0 = t;
  if (end > t0) {
    const std::ptrdiff_t w_end = end + im.gate_k[3] + 2;  // window sums needed: [t0, w_end)
    float* ws = im.wsum.data();
    if (im.win == 2) {  // 2.0-2.9 MS/s: a fixed width the compiler vectorizes
      for (std::ptrdiff_t k = t0; k < w_end; ++k) ws[k] = m[k] + m[k + 1];
    } else {
      for (std::ptrdiff_t k = t0; k < w_end; ++k) {
        float acc = 0.0f;
        for (std::ptrdiff_t j = 0; j < im.win; ++j) acc += m[k + j];
        ws[k] = acc;
      }
    }
    // The larger of the two windows a pulse can sit in, once per position
    // (in place: ws[k] only needs ws[k + 1], which is read before it changes).
    for (std::ptrdiff_t k = t0; k + 1 < w_end; ++k) ws[k] = std::max(ws[k], ws[k + 1]);
    std::uint8_t* g = im.gate.data();
    const std::ptrdiff_t k0 = im.gate_k[0], k1 = im.gate_k[1], k2 = im.gate_k[2], k3 = im.gate_k[3];
    // One threshold per noise block.
    for (std::ptrdiff_t seg = t0; seg < end;) {
      const std::uint64_t blk = (base_ + static_cast<std::uint64_t>(seg)) / kNoiseBlock;
      const auto seg_end = std::min<std::ptrdiff_t>(end, static_cast<std::ptrdiff_t>((blk + 1) * kNoiseBlock - base_));
      const float gate = im.gate_scale * noise_for(blk, final);
      for (std::ptrdiff_t u = seg; u < seg_end; ++u) {
        g[u - t0] = static_cast<std::uint8_t>((ws[u + k0] > gate) & (ws[u + k1] > gate) & (ws[u + k2] > gate) &
                                              (ws[u + k3] > gate));
      }
      seg = seg_end;
    }
  }
  std::uint64_t noise_blk = ~std::uint64_t{0};
  float noise = 0.0f, min_signal = 0.0f;

  for (; t < end; ++t) {
    if (!im.gate[static_cast<std::size_t>(t - t0)]) {
      // Most positions are closed: jump to the next open one.
      const void* hit = std::memchr(im.gate.data() + (t - t0), 1, static_cast<std::size_t>(end - t));
      if (hit == nullptr) {
        t = end;
        break;
      }
      t = t0 + (static_cast<const std::uint8_t*>(hit) - im.gate.data());
    }
    ++stats_.gated;
    if (const std::uint64_t blk = (base_ + static_cast<std::uint64_t>(t)) / kNoiseBlock; blk != noise_blk) {
      noise_blk = blk;
      noise = noise_for(blk, final);
      min_signal = static_cast<float>(cfg_.min_signal_ratio) * noise;
    }

    // Steps 2-4 per phase.
    struct Best {
      int score = 0;
      float margin = 0.0f;
      int phase = 0;
      float signal = 0.0f;
      std::uint8_t bytes[14] = {};
      int bits = 0;
      std::uint32_t icao = 0;
      int corrected = -1;
    } best;
    bool any_preamble = false;
    bool saw_unknown_address = false;
    int seq_phase = -1;
    float seq_quality = 0.0f;
    for (int p = 0; p < im.phases; ++p) {
      float pulse[4];
      float pmin = 1e30f, psum = 0.0f;
      for (int i = 0; i < 4; ++i) {
        pulse[i] = im.chip(m, t, p, kPulseChips[i]);
        pmin = std::min(pmin, pulse[i]);
        psum += pulse[i];
      }
      const float signal = psum / (4.0f * chip_len);
      if (signal < min_signal) continue;
      float qsum = 0.0f;
      for (int c : kQuietChips) qsum += im.chip(m, t, p, c);
      const float quiet = qsum / 12.0f;
      if (pmin < quiet * pulse_quiet) continue;
      any_preamble = true;

      // Step 3: bits.
      std::uint8_t bytes[14] = {};
      float margin = 0.0f;
      auto bit = [&](int i) {
        const float a = im.chip(m, t, p, kPreambleChips + 2 * i);
        const float b = im.chip(m, t, p, kPreambleChips + 2 * i + 1);
        margin += std::fabs(a - b);
        if (a > b) bytes[i / 8] = static_cast<std::uint8_t>(bytes[i / 8] | (0x80u >> (i % 8)));
      };
      // At the end of the stream the message must end inside the data.
      auto fits = [&](int nbits) {
        return !final || static_cast<double>(t) + 2.0 + (kPreambleChips + 2 * nbits) * spu_ / 2 <= static_cast<double>(valid);
      };

      // Step 4: check. Scores: 4 clean; 3 DF11 with an interrogator code (a
      // nonzero syndrome below 80: also what one wrong bit in the last 7
      // parity bits looks like, so a phase that reads it clean wins); 2 one
      // bit fixed; 1 Address/Parity with a known address; 0 rejected.
      std::uint32_t icao = 0;
      int corrected = -1;
      bool unknown = false;
      auto check = [&](std::uint8_t* fb, int nbits) {
        const unsigned fdf = fb[0] >> 3;
        const std::uint32_t syn = modes::crc24_syndrome(fb, static_cast<std::size_t>(nbits / 8));
        corrected = -1;
        unknown = false;
        if (fdf == 17 || fdf == 18 || fdf == 11) {
          auto address = [&] { icao = std::uint32_t{fb[1]} << 16 | std::uint32_t{fb[2]} << 8 | fb[3]; };
          address();
          if (syn == 0) return 4;
          if (fdf == 11 && syn < 80) return 3;
          if (fix) {
            const std::uint32_t* tab = nbits == 56 ? im.syn56 : im.syn112;
            for (int i = 5; i < nbits; ++i) {
              if (tab[i] == syn) {
                fb[i / 8] = static_cast<std::uint8_t>(fb[i / 8] ^ (0x80u >> (i % 8)));
                corrected = i;
                address();  // the bit may have been in the address
                return 2;
              }
            }
          }
          return 0;
        }
        icao = syn;
        if (im.addresses.contains(syn, base_ + static_cast<std::uint64_t>(t), im.ttl)) return 1;
        unknown = true;
        return 0;
      };

      for (int i = 0; i < 5; ++i) bit(i);
      unsigned df = bytes[0] >> 3;
      int bits = df >= 16 ? 112 : 56;
      int score = 0;
      if (supported_df(df) && fits(bits)) {
        for (int i = 5; i < bits; ++i) bit(i);
        score = check(bytes, bits);
      }
      // Remember the most promising phase that failed, for step 3b.
      if (score == 0 && signal - quiet > seq_quality) {
        seq_quality = signal - quiet;
        seq_phase = p;
      }
      if (score == 0) {
        if (unknown) saw_unknown_address = true;
        continue;
      }
      margin /= static_cast<float>(bits) * signal * chip_len;
      if (score > best.score || (score == best.score && score > 0 && margin > best.margin)) {
        best.score = score;
        best.margin = margin;
        best.phase = p;
        best.signal = signal;
        std::memcpy(best.bytes, bytes, sizeof bytes);
        best.bits = bits;
        best.icao = icao;
        best.corrected = corrected;
      }
    }
    if (!any_preamble) continue;
    ++stats_.preambles;
    // Step 3b: no phase gave a valid frame. Run the sequence detector once,
    // on the failed phase with the clearest preamble (pulses over quiet),
    // for both frame lengths. It accepts only clean CRCs and known
    // Address/Parity addresses (not DF11 interrogator codes or fixed bits:
    // it sees many more candidates than frames, and those would let
    // near-misses through).
    if (best.score == 0 && seq_phase >= 0 && cfg_.sequence_detector) {
      auto fits_bits = [&](int nbits) {
        return !final || static_cast<double>(t) + 2.0 + (kPreambleChips + 2 * nbits) * spu_ / 2 <= static_cast<double>(valid);
      };
      std::uint8_t seq112[14], seq56[14];
      const int run = fits_bits(112) ? 112 : (fits_bits(56) ? 56 : 0);
      if (run > 0 && im.detect_sequence(m + t, seq_phase, run, run == 112 ? seq112 : seq56, run == 112 ? seq56 : nullptr)) {
        for (int nbits : {112, 56}) {
          if (nbits > run) continue;
          std::uint8_t* seq = nbits == 112 ? seq112 : seq56;
          const unsigned sdf = seq[0] >> 3;
          if (!supported_df(sdf) || (sdf >= 16 ? 112 : 56) != nbits) continue;
          const std::uint32_t syn = modes::crc24_syndrome(seq, static_cast<std::size_t>(nbits / 8));
          int sc = 0;
          std::uint32_t sicao = syn;
          if (sdf == 17 || sdf == 18 || sdf == 11) {
            sicao = std::uint32_t{seq[1]} << 16 | std::uint32_t{seq[2]} << 8 | seq[3];
            if (syn == 0) sc = 4;
          } else if (im.addresses.contains(syn, base_ + static_cast<std::uint64_t>(t), im.ttl)) {
            sc = 1;
          }
          if (sc > 0) {
            best.score = sc;
            best.phase = seq_phase;
            std::memcpy(best.bytes, seq, 14);
            best.bits = nbits;
            best.icao = sicao;
            best.corrected = -1;
            best.margin = 0.0f;
            ++stats_.sequence_decoded;
            break;
          }
        }
      }
    }
    if (best.score == 0) {
      ++stats_.bad_crc;
      if (saw_unknown_address) ++stats_.unknown_address;
      continue;
    }

    // Step 5: timing. A matched filter: for a trial start the expected
    // magnitude of every sample is e_k = sum over chips of sign * (overlap
    // of the chip with the sample), and the score is <m, e> / |e|. Signs:
    // data chips +-1 from the bits, preamble pulses +1 and the four quiet
    // chips right after them -1, so the template sums to zero and the noise
    // floor drops out; dividing by |e| removes the dependence of |e| on the
    // sampling phase, so the peak sits at the true start. Trial starts on a
    // 0.1-sample grid within +-0.3 samples of the winning phase, then a
    // parabola through the peak. All positions are relative to t, so the
    // result does not depend on where t sits in the buffer.
    const float* mt = m + t;
    const double rel0 = static_cast<double>(best.phase) / im.phases;
    const int nchips = kPreambleChips + 2 * best.bits;
    std::int8_t sign[kMaxChips] = {};
    for (int c : kPulseChips) sign[c] = 1;
    for (int c : {1, 3, 8, 10}) sign[c] = -1;
    for (int i = 0; i < best.bits; ++i) {
      const bool one = (best.bytes[i / 8] >> (7 - i % 8)) & 1;
      sign[kPreambleChips + 2 * i] = one ? 1 : -1;
      sign[kPreambleChips + 2 * i + 1] = one ? -1 : 1;
    }
    // 0.04 us steps (0.096 samples at 2.4 MS/s) up to +-0.5 us: at high
    // rates the first passing phase can be more than a sample early.
    const double kStep = kClimbStepUs * spu_;
    constexpr int kMaxSteps = kClimbSteps;
    const std::ptrdiff_t off = im.back;  // e[k + off] holds sample k (k >= -back)
    const double h = spu_ / 2;
    std::vector<double>& e = im.templ;
    // Score <m, e> / |e| and amplitude 2 <m, e> / |e|^2 (m ~ A/2 e + const).
    auto evaluate = [&](double rel, double& amp) {
      // Samples k (relative to t) cover [k - 0.5, k + 0.5); stored at e[k + off].
      const auto k_lo = static_cast<std::ptrdiff_t>(std::floor(rel + 0.5)) - 1;
      const auto k_hi = static_cast<std::ptrdiff_t>(std::floor(rel + nchips * h + 0.5)) + 1;
      // The margins (back, need, templ's size) cover the climb's reach; a
      // trial outside them would touch memory it must not, so it never wins.
      if (k_lo + off < 0 || k_lo < -static_cast<std::ptrdiff_t>(t) ||
          static_cast<std::size_t>(k_hi + off + 1) > e.size() || t + k_hi >= static_cast<std::ptrdiff_t>(valid_) + static_cast<std::ptrdiff_t>(final ? im.need + 8 : 0)) {
        amp = 0.0;
        return -std::numeric_limits<double>::infinity();
      }
      std::fill(e.begin() + (k_lo + off), e.begin() + (k_hi + off + 1), 0.0);
      for (int c = 0; c < nchips; ++c) {
        if (sign[c] == 0) continue;
        const double a = rel + c * h, bnd = a + h;
        for (auto k = static_cast<std::ptrdiff_t>(std::floor(a + 0.5)); static_cast<double>(k) - 0.5 < bnd; ++k) {
          const double ov = std::min(bnd, static_cast<double>(k) + 0.5) - std::max(a, static_cast<double>(k) - 0.5);
          if (ov > 0) e[static_cast<std::size_t>(k + off)] += sign[c] * ov;
        }
      }
      double dot = 0.0, norm = 0.0;
      for (std::ptrdiff_t k = k_lo; k <= k_hi; ++k) {
        const double v = e[static_cast<std::size_t>(k + off)];
        dot += static_cast<double>(mt[k]) * v;
        norm += v * v;
      }
      amp = norm > 0 ? 2.0 * dot / norm : 0.0;
      return norm > 0 ? dot / std::sqrt(norm) : 0.0;
    };
    // Hill climb from the winning phase in 0.04 us steps (the first
    // position whose phase passes is not always the closest one), then a
    // parabola through the peak and its neighbours.
    // Each trial is evaluated once: a step reuses the neighbour it moves to
    // (score and amplitude) and evaluates only the new one beyond it.
    double amp_c = 0.0, amp_m = 0.0, amp_p = 0.0;
    int jc = 0;
    double yc = evaluate(rel0, amp_c);
    double ym = evaluate(rel0 - kStep, amp_m);
    double yp = evaluate(rel0 + kStep, amp_p);
    const int dir = yp > yc ? 1 : (ym > yc ? -1 : 0);
    while (dir != 0 && std::abs(jc) < kMaxSteps) {
      const double ahead = dir > 0 ? yp : ym;
      if (!(ahead > yc)) break;
      jc += dir;
      if (dir > 0) {
        ym = yc;
        amp_m = amp_c;
        yc = yp;
        amp_c = amp_p;
        yp = evaluate(rel0 + (jc + 1) * kStep, amp_p);
      } else {
        yp = yc;
        amp_p = amp_c;
        yc = ym;
        amp_c = amp_m;
        ym = evaluate(rel0 + (jc - 1) * kStep, amp_m);
      }
    }
    double rel_best = rel0 + jc * kStep;
    {
      const double den = ym - 2 * yc + yp;
      // yc is not below its neighbours, so the vertex lies within half a step.
      if (den < 0 && yc >= ym && yc >= yp) rel_best += kStep * std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
    }
    const double amplitude = amp_c;
    const double s_best = static_cast<double>(t) + rel_best;

    if (n_out_ == out_.size()) break;  // cannot happen: out_ is sized for the block
    DemodFrame& f = out_[n_out_++];
    std::memcpy(f.bytes, best.bytes, 14);
    f.len = static_cast<std::uint8_t>(best.bits / 8);
    f.df = static_cast<std::uint8_t>(best.bytes[0] >> 3);
    f.icao = best.icao;
    f.sample_pos = static_cast<double>(base_ + static_cast<std::uint64_t>(t)) + rel_best;
    f.signal = static_cast<float>(amplitude);
    f.noise = noise;
    f.snr_db = noise > 0 && amplitude > 0 ? 20.0f * std::log10(static_cast<float>(amplitude) / noise) : 99.0f;
    f.margin = best.margin;
    f.corrected_bit = static_cast<std::int8_t>(best.corrected);
    ++stats_.frames;
    ++stats_.by_df[f.df];
    if (best.corrected >= 0) ++stats_.corrected;
    // A verified DF11/17/18 address makes Address/Parity replies from it acceptable.
    if (best.score >= 2) im.addresses.add(best.icao, base_ + static_cast<std::uint64_t>(t));
    // Resume shortly before the message's end: the next reply's preamble
    // may overlap its last microseconds (a busy channel). Less than the
    // shortest message, so the same message is never found twice.
    const double resume = s_best + nchips * spu_ / 2 - cfg_.resume_overlap_us * spu_;
    t = std::max(t, static_cast<std::ptrdiff_t>(std::floor(resume)) - 1);
  }
  if (t < im.back) t = im.back;

  // Keep the samples the next scan needs.
  const std::ptrdiff_t keep_from = final ? valid : std::min(t - im.back, valid);
  next_ = base_ + static_cast<std::uint64_t>(std::max<std::ptrdiff_t>(t, 0));
  if (keep_from > 0) {
    const std::size_t keep = valid_ - static_cast<std::size_t>(keep_from);
    std::memmove(buf.data(), buf.data() + keep_from, keep * sizeof(float));
    base_ += static_cast<std::uint64_t>(keep_from);
    valid_ = keep;
  }
}

std::size_t to_beast(const DemodFrame& f, double sample_rate_hz, std::vector<std::uint8_t>& out) {
  const std::size_t before = out.size();
  out.push_back(0x1A);
  out.push_back(f.len == 7 ? '2' : '3');
  std::uint8_t body[7 + 14];
  const double ts_d = std::max(0.0, f.sample_pos) * 12e6 / sample_rate_hz;
  const auto ts = static_cast<std::uint64_t>(std::llround(ts_d)) & 0xFFFFFFFFFFFFull;
  for (int i = 0; i < 6; ++i) body[i] = static_cast<std::uint8_t>(ts >> (40 - 8 * i));
  body[6] = static_cast<std::uint8_t>(std::clamp(std::lround(f.signal * 255.0f), 0L, 255L));
  std::memcpy(body + 7, f.bytes, f.len);
  for (std::size_t i = 0; i < 7u + f.len; ++i) {
    out.push_back(body[i]);
    if (body[i] == 0x1A) out.push_back(0x1A);
  }
  return out.size() - before;
}

}  // namespace adsb::demod
