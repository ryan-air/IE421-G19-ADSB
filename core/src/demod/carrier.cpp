#include "adsb/demod/carrier.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "adsb/common/portable_math.hpp"

namespace adsb::demod {

namespace pm = adsb::pm;

bool validate(const CarrierConfig& cfg, std::string* why) {
  auto fail = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  if (!std::isfinite(cfg.sample_rate_hz) || cfg.sample_rate_hz < 1e6 || cfg.sample_rate_hz > 1e8)
    return fail("sample rate must be 1-100 MS/s");
  if (!std::isfinite(cfg.min_cover) || cfg.min_cover <= 0.0 || cfg.min_cover > 1.0)
    return fail("min_cover must be in (0, 1]");
  if (cfg.fine_iterations < 1 || cfg.fine_iterations > 16) return fail("fine_iterations must be 1-16");
  if (cfg.min_samples < 3 || cfg.min_samples > 100000) return fail("min_samples must be 3-100000");
  if (!(cfg.clip_level > 0.0)) return fail("clip_level must be > 0");
  return true;
}

void to_complex(const std::uint8_t* data, std::size_t n, SampleFormat f, float* out) noexcept {
  switch (f) {
    case SampleFormat::kCu8:
      for (std::size_t k = 0; k < 2 * n; ++k) out[k] = (static_cast<float>(data[k]) - 127.5f) * (1.0f / 127.5f);
      return;
    case SampleFormat::kCs8:
      for (std::size_t k = 0; k < 2 * n; ++k)
        out[k] = static_cast<float>(static_cast<std::int8_t>(data[k])) * (1.0f / 128.0f);
      return;
    case SampleFormat::kCs16:
      for (std::size_t k = 0; k < 2 * n; ++k) {
        const auto v = static_cast<std::int16_t>(static_cast<std::uint16_t>(data[2 * k] | data[2 * k + 1] << 8));
        out[k] = static_cast<float>(v) * (1.0f / 32768.0f);
      }
      return;
    case SampleFormat::kCf32:
      std::memcpy(out, data, 8 * n);
      for (std::size_t k = 0; k < 2 * n; ++k)
        if (!std::isfinite(out[k])) out[k] = 0.0f;  // as the demodulator: silence
      return;
  }
}

int CarrierEstimator::pulse_intervals_us(const std::uint8_t* bytes, int len, double* a, double* b) noexcept {
  static constexpr double kPre[4] = {0.0, 1.0, 3.5, 4.5};
  int n = 0;
  for (double p : kPre) {
    a[n] = p;
    b[n] = p + 0.5;
    ++n;
  }
  for (int i = 0; i < 8 * len; ++i) {
    const bool one = ((static_cast<unsigned>(bytes[i / 8]) >> static_cast<unsigned>(7 - i % 8)) & 1u) != 0;
    a[n] = 8.0 + i + (one ? 0.0 : 0.5);
    b[n] = a[n] + 0.5;
    ++n;
  }
  return n;
}

CarrierEstimator::CarrierEstimator(const CarrierConfig& cfg) : cfg_(cfg), spu_(cfg.sample_rate_hz / 1e6) {
  // The longest message spans 120 us; a sample more on each side.
  const auto most = static_cast<std::size_t>(std::ceil(120.0 * spu_)) + 4;
  re_.resize(most);
  im_.resize(most);
  t_.resize(most);
  w_.resize(most);
  idx_.resize(most);
  std::size_t nfft = 1;
  while (nfft < 4 * most) nfft <<= 1;
  fft_re_.resize(nfft);
  fft_im_.resize(nfft);
  tw_re_.resize(nfft / 2);
  tw_im_.resize(nfft / 2);
  for (std::size_t k = 0; k < nfft / 2; ++k) {
    double s, c;
    pm::sincos(-pm::kTwoPi * static_cast<double>(k) / static_cast<double>(nfft), s, c);
    tw_re_[k] = c;
    tw_im_[k] = s;
  }
}

// In-place radix-2 FFT of the first n (a power of two, at most the table
// size) values of fft_re_ / fft_im_.
void CarrierEstimator::fft(std::size_t n) noexcept {
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(fft_re_[i], fft_re_[j]);
      std::swap(fft_im_[i], fft_im_[j]);
    }
  }
  const std::size_t table = tw_re_.size() * 2;
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const std::size_t stride = table / len;
    for (std::size_t i = 0; i < n; i += len) {
      for (std::size_t k = 0; k < len / 2; ++k) {
        const double wr = tw_re_[k * stride], wi = tw_im_[k * stride];
        const std::size_t u = i + k, v = i + k + len / 2;
        const double xr = fft_re_[v] * wr - fft_im_[v] * wi;
        const double xi = fft_re_[v] * wi + fft_im_[v] * wr;
        fft_re_[v] = fft_re_[u] - xr;
        fft_im_[v] = fft_im_[u] - xi;
        fft_re_[u] += xr;
        fft_im_[u] += xi;
      }
    }
  }
}

CarrierEstimate CarrierEstimator::estimate(const float* iq, std::int64_t base, std::size_t n, double sample_pos,
                                           const std::uint8_t* bytes, int len) {
  CarrierEstimate e;
  if (len != 7 && len != 14) return e;
  if (!std::isfinite(sample_pos)) return e;
  double pa[4 + 112], pb[4 + 112];
  const int np = pulse_intervals_us(bytes, len, pa, pb);
  for (int i = 0; i < np; ++i) {
    pa[i] = sample_pos + pa[i] * spu_;
    pb[i] = sample_pos + pb[i] * spu_;
  }
  const auto k0 = static_cast<std::int64_t>(std::floor(pa[0] + 0.5));
  const auto k1 = static_cast<std::int64_t>(std::ceil(pb[np - 1] + 0.5)) - 1;
  if (k0 < base || k1 >= base + static_cast<std::int64_t>(n)) return e;

  // Weight and covered middle of every sample the pulses touch.
  std::size_t m = 0;
  int p = 0;
  const double ts = 1.0 / cfg_.sample_rate_hz;
  for (std::int64_t k = k0; k <= k1 && m < re_.size(); ++k) {
    const double lo = static_cast<double>(k) - 0.5;
    const double hi = lo + 1.0;
    while (p < np && pb[p] <= lo) ++p;
    double cover = 0.0, mid = 0.0;
    for (int q = p; q < np && pa[q] < hi; ++q) {
      const double l = std::max(lo, pa[q]);
      const double h = std::min(hi, pb[q]);
      if (h > l) {
        cover += h - l;
        mid += (h - l) * (l + h) * 0.5;
      }
    }
    if (cover < cfg_.min_cover) continue;
    mid /= cover;
    const auto off = static_cast<std::size_t>(k - base);
    const double vi = static_cast<double>(iq[2 * off]), vq = static_cast<double>(iq[2 * off + 1]);
    // Clipped, or marked unusable by the caller (non-finite: adsb-carrier
    // marks samples clipped before it removes the DC offset).
    if (!std::isfinite(vi) || !std::isfinite(vq) || std::fabs(vi) >= cfg_.clip_level ||
        std::fabs(vq) >= cfg_.clip_level) {
      ++e.clipped;
      continue;
    }
    re_[m] = cover * vi;
    im_[m] = cover * vq;
    w_[m] = cover;
    t_[m] = (mid - sample_pos) * ts;
    idx_[m] = k;
    ++m;
  }
  e.samples = static_cast<int>(m);
  if (static_cast<int>(m) < cfg_.min_samples) return e;

  // 1. Coarse: the periodogram of the weighted samples on the sample grid
  // (zero-padded FFT, at least 4x the span), over the whole band.
  const auto span_samples = static_cast<std::size_t>(idx_[m - 1] - idx_[0] + 1);
  std::size_t nfft = 1;
  while (nfft < 4 * span_samples) nfft <<= 1;
  nfft = std::min(nfft, fft_re_.size());
  std::fill(fft_re_.begin(), fft_re_.begin() + static_cast<std::ptrdiff_t>(nfft), 0.0);
  std::fill(fft_im_.begin(), fft_im_.begin() + static_cast<std::ptrdiff_t>(nfft), 0.0);
  for (std::size_t i = 0; i < m; ++i) {
    const auto j = static_cast<std::size_t>(idx_[i] - idx_[0]);
    fft_re_[j] = re_[i];
    fft_im_[j] = im_[i];
  }
  fft(nfft);
  std::size_t peak = 0;
  double peak_p = -1.0;
  for (std::size_t j = 0; j < nfft; ++j) {
    const double pw = fft_re_[j] * fft_re_[j] + fft_im_[j] * fft_im_[j];
    if (pw > peak_p) {
      peak_p = pw;
      peak = j;
    }
  }
  if (!(peak_p > 0.0)) return e;  // silence
  const double bin = cfg_.sample_rate_hz / static_cast<double>(nfft);
  const double fc =
      (peak < nfft / 2 ? static_cast<double>(peak) : static_cast<double>(peak) - static_cast<double>(nfft)) * bin;
  e.coarse_hz = fc;

  // |sum w z exp(-j 2 pi f t)|^2 at the samples' covered middles
  auto power = [&](double f) {
    double sr = 0.0, si = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
      double s, c;
      pm::sincos(-pm::kTwoPi * f * t_[i], s, c);
      sr += re_[i] * c - im_[i] * s;
      si += re_[i] * s + im_[i] * c;
    }
    return sr * sr + si * si;
  };

  // 2. Grid of 1/(4 T) over two FFT bins on each side, on the covered
  // middles (partly covered samples sit off the sample grid).
  const double span = std::max(t_[m - 1] - t_[0], ts);
  const double step = 1.0 / (4.0 * span);
  const int half = static_cast<int>(std::ceil(2.0 * bin / step));
  // The best point is within half a step (a quarter cycle over the span) of
  // the peak, close enough for the fine fit.
  double best_f = fc, best_p = -1.0;
  for (int j = -half; j <= half; ++j) {
    const double f = fc + j * step;
    const double pw = power(f);
    if (pw > best_p) {
      best_p = pw;
      best_f = f;
    }
  }
  // 3. Fine: weighted least squares of the residual phase against time.
  double f = best_f;
  double sum_r = 0.0, sum_i = 0.0, sum_abs = 0.0, rms = 0.0, sigma = 0.0;
  for (int it = 0; it <= cfg_.fine_iterations; ++it) {
    // Residual terms r_i = w z exp(-j 2 pi f t), their sum S.
    sum_r = sum_i = sum_abs = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
      double s, c;
      pm::sincos(-pm::kTwoPi * f * t_[i], s, c);
      const double ar = re_[i] * c - im_[i] * s;
      const double ai = re_[i] * s + im_[i] * c;
      sum_r += ar;
      sum_i += ai;
      sum_abs += std::sqrt(ar * ar + ai * ai);
    }
    // Each term's angle relative to S, weighted by |r|^2, fitted against time.
    double sw = 0.0, sw2 = 0.0, st = 0.0, sa = 0.0, stt = 0.0, sta = 0.0, saa = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
      double s, c;
      pm::sincos(-pm::kTwoPi * f * t_[i], s, c);
      const double ar = re_[i] * c - im_[i] * s;
      const double ai = re_[i] * s + im_[i] * c;
      // r conj(S)
      const double xr = ar * sum_r + ai * sum_i;
      const double xi = ai * sum_r - ar * sum_i;
      const double ang = pm::atan2(xi, xr);
      const double wt = ar * ar + ai * ai;
      sw += wt;
      sw2 += wt * wt;
      st += wt * t_[i];
      sa += wt * ang;
      stt += wt * t_[i] * t_[i];
      sta += wt * t_[i] * ang;
      saa += wt * ang * ang;
    }
    if (sw <= 0.0) return e;
    const double tm = st / sw;
    const double am = sa / sw;
    const double vtt = stt / sw - tm * tm;
    const double vta = sta / sw - tm * am;
    const double vaa = saa / sw - am * am;
    if (vtt <= 0.0) return e;
    const double slope = vta / vtt;  // rad/s
    // Standard error of the slope: residual variance over the effective
    // number of samples, (sum w)^2 / sum w^2 (fewer than m when the weights
    // are unequal).
    const double res = std::max(0.0, vaa - slope * vta);
    rms = std::sqrt(res);
    const double n_eff = sw * sw / sw2;
    if (n_eff <= 3.0) return e;
    sigma = std::sqrt(res / ((n_eff - 2.0) * vtt)) / pm::kTwoPi;
    if (it == cfg_.fine_iterations) break;  // the last pass only measures
    f += slope / pm::kTwoPi;
  }
  // The fit refines the grid's peak; a step far beyond the grid spacing is
  // not a refinement (a few samples dominating the weights): no estimate.
  e.ok = std::isfinite(f) && std::isfinite(sigma) && std::fabs(f - best_f) <= 2.0 * step;
  e.freq_hz = f;
  e.sigma_hz = sigma;
  e.coherence = sum_abs > 0.0 ? std::sqrt(sum_r * sum_r + sum_i * sum_i) / sum_abs : 0.0;
  e.phase_rms_rad = rms;
  return e;
}

}  // namespace adsb::demod
