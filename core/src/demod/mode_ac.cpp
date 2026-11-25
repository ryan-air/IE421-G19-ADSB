// Mode A/C reply detection for the demodulator (DemodConfig::mode_ac).
//
// Runs after the Mode S scan over the same buffered magnitudes, with the
// same noise floor, and leaves the Mode S path untouched. A reply is up to
// 20 pulse slots 1.45 us apart (modes/mode_ac.hpp). Per sample position t,
// for kAcPhases fractional starts s = t + p / kAcPhases:
//
//  1. Gate (whole range at once, vectorizable): windows that hold all of F1
//     and of F2 for any start in [t, t + 1) must both sum to a share of what
//     a pulse at ac_min_signal_ratio gives, and those of the always-empty X
//     slot and slot 15 to less than 3/4 of their mean (inside Mode S
//     traffic they nearly always hold chip energy, so this removes most
//     positions there).
//  2. Mode S: a position whose reply would overlap a decoded Mode S frame
//     (from 1 us before F1 to the end of SPI) is skipped. Both would be
//     garbled; Mode S has a CRC, the Mode A/C reply has none.
//  3. Framing: the mean magnitude over each framing pulse (exact sample
//     overlap, as for the Mode S chips) must reach ac_min_signal_ratio
//     times the noise floor, and the two must agree within
//     ac_framing_ratio. Their mean is the reply's level A.
//  4. Slots and gaps: every other slot's level, as a share x of the way
//     from the noise floor to what a pulse of amplitude A gives in that
//     slot (Win::gain), is a pulse (x >= 0.5 + band) or empty
//     (x <= 0.5 - band). The reply is rejected as garbled if any slot is
//     uncertain (in between) or too strong (x > ac_max_pulse: another reply
//     on top), if X or an always-empty slot after F2 holds a pulse, or if a
//     gap between the reply's slots up to its last pulse (F2, or SPI; 0.05
//     us in from either end) or the 0.5 us ending 0.5 us before F1 holds
//     more than ac_max_gap of A above the noise floor and what the
//     neighbouring pulses leak into it through shared samples (a reply
//     offset by anything but a multiple of 1.45 us puts its pulses there).
//  5. The best of the passing phases of t and t + 1 (largest framing
//     amplitudes) wins; its start is refined by a hill climb (0.04 us
//     steps) on the matched filter of its whole pulse template and a
//     parabola through the peak.
//  6. That template (least-squares amplitude) must explain the samples
//     from 0.5 us before F1 to 0.5 us after its last pulse to within the
//     noise plus ac_max_residual pulse energies (residual_ok): this catches
//     an overlapping pulse that straddles a slot's edge, whose energy the
//     slot and gap windows split between them at 2.4 MS/s. Scanning
//     resumes after F2.
//
// Overlapping replies whose pulses line up (offsets that are multiples of
// 1.45 us, "synchronous garble") are indistinguishable from one reply with
// both codes' pulses, as for any SSR receiver; where their levels differ
// the too-strong and uncertain checks catch most of them.
//
// Frames are merged with the Mode S frames in time order. Positions within
// `lag` of the Mode S scan's end wait for the next call, so that every
// Mode S frame that could overlap them is known; frames after the merge
// point are held back until then.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "adsb/demod/demod.hpp"
#include "adsb/modes/mode_ac.hpp"

namespace adsb::demod {

namespace {

constexpr int kAcPhases = 4;
constexpr int kAcSlots = modes::kModeAcSlots;        // 20
constexpr int kAcGaps = kAcSlots - 1;                // 19
constexpr int kGapWin = kAcSlots;                    // windows 20..38: the gaps
constexpr int kPreWin = kAcSlots + kAcGaps;          // window 39: before F1
constexpr int kAcWindows = kPreWin + 1;
constexpr double kGapHalfUs = 0.45;                  // a 1.0 us gap but 0.05 us at either end
constexpr double kPreFromUs = -1.0, kPreToUs = -0.5;
constexpr double kResidualMarginUs = 0.5;            // the template residual's span around the pulses
constexpr double kExcludeBeforeUs = 1.0;             // a Mode S frame ending this soon before F1 overlaps
constexpr double kReplyMaxUs = modes::kModeAcSpiSlot * modes::kModeAcSlotUs + modes::kModeAcPulseUs;  // 25.1
constexpr double kGateShare = 0.8;                   // of the weakest accepted pulse's window sum
constexpr float kGateXShare = 0.75f;                 // X's and slot 15's window sums below this share of the framing ones
constexpr double kClimbStepUs = 0.04;                // timing hill climb step
constexpr int kClimbSteps = 13;                      // ... and its reach either way (0.52 us)

// Slots other than the framing pulses, in the order they are judged: X and
// the empty slots after F2, then the code pulses and SPI.
constexpr int kSlotOrder[] = {7, 15, 16, 18, 19, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 17};

struct AcCandidate {
  bool framing = false;   // passed step 3
  bool ok = false;        // passed step 4
  float score = 0.0f;     // framing amplitudes (largest when aligned)
  float amp = 0.0f;
  float margin = 0.0f;
  std::uint32_t pulses = 0;
};

}  // namespace

struct Demodulator::ModeAc {
  struct Win {
    std::int32_t first = 0;   // first sample, relative to t
    std::uint32_t off = 0;    // into w
    std::uint32_t count = 0;
    float inv_width = 0.0f;   // 1 / window length in samples
    // Level of a window that holds exactly a pulse of amplitude 1. Sample j
    // of a rectangular pulse has magnitude A ov_j (its share of the pulse),
    // so the window gives A sum(ov_j^2) / width: from about 0.5 A (a pulse
    // split over two samples) to A, depending on the pulse's sampling phase,
    // which differs from slot to slot (1.45 us is not a whole number of
    // samples). Dividing by it makes the slots comparable.
    float gain = 1.0f;
    // Gaps and the pre window: the level a pulse of amplitude 1 in the slot
    // before (after) the window puts into it through the samples they
    // share (at 2.4 MS/s a sample is 0.42 us wide).
    float leak_prev = 0.0f, leak_next = 0.0f;
  };
  std::array<Win, kAcPhases * kAcWindows> win{};
  std::vector<float> w;
  std::ptrdiff_t back = 0;    // samples read before a position
  std::ptrdiff_t lag = 0;     // positions this close to the Mode S scan's end wait
  double f2_end = 0.0;        // samples from F1 to the end of F2 (resume point)
  double exclude_before = 0.0, exclude_after = 0.0;  // Mode S overlap test, samples
  // Gate.
  std::ptrdiff_t gwin = 2, g14 = 0, g7 = 0, g15 = 0;
  float gate_scale = 0.0f;
  std::vector<float> ws;
  std::vector<std::uint8_t> gate;
  // Thresholds.
  float min_ratio = 3.0f, framing_ratio = 2.0f, off_max = 0.4f, on_min = 0.6f, max_pulse = 1.6f, gap_max = 0.2f;
  // State across calls.
  std::uint64_t next = 0;     // next position to scan (absolute)
  double modes_end = -std::numeric_limits<double>::infinity();  // latest end of an emitted Mode S frame
  std::vector<DemodFrame> pending;  // frames held for the next call, in time order
  std::size_t n_pending = 0;
  std::vector<DemodFrame> modes;    // scratch: held + new Mode S frames
  std::vector<DemodFrame> found;    // scratch: Mode A/C replies of this call
  std::vector<double> xs;           // scratch: pulse overlap per sample (residual_ok)
  double max_excess = 0.3;

  [[nodiscard]] float level(const float* m, std::ptrdiff_t t, int p, int k) const noexcept {
    const Win& x = win[static_cast<std::size_t>(p * kAcWindows + k)];
    const float* wp = w.data() + x.off;
    const float* xp = m + t + x.first;
    float s = 0.0f;
    for (std::uint32_t j = 0; j < x.count; ++j) s += wp[j] * xp[j];
    return s * x.inv_width;
  }

  [[nodiscard]] float gain(int p, int k) const noexcept { return win[static_cast<std::size_t>(p * kAcWindows + k)].gain; }

  [[nodiscard]] AcCandidate evaluate(const float* m, std::ptrdiff_t t, int p, float noise) const noexcept {
    AcCandidate r;
    // The framing pulses' levels must clear the noise floor as they are
    // (a low-gain window holds less of the pulse, but just as much noise);
    // their amplitudes (level / gain) must agree.
    const float r0 = level(m, t, p, 0), r14 = level(m, t, p, modes::kModeAcF2Slot);
    if (!(std::min(r0, r14) >= min_ratio * noise)) return r;
    const float l0 = r0 / gain(p, 0);
    const float l14 = r14 / gain(p, modes::kModeAcF2Slot);
    const float lo = std::min(l0, l14), hi = std::max(l0, l14);
    if (hi > framing_ratio * lo) return r;
    r.framing = true;
    const float amp = 0.5f * (l0 + l14);
    if (!(amp > noise)) return r;
    const float inv = 1.0f / (amp - noise);
    float margin = 0.5f;
    std::uint32_t pulses = 1u | (1u << modes::kModeAcF2Slot);
    // The always-empty slots first: inside other traffic (Mode S chips every
    // 0.5 us) one of them usually holds a pulse, which ends the work early.
    for (const int k : kSlotOrder) {
      // Of the way from the noise floor to what a pulse would give here.
      const float full = amp * gain(p, k) - noise;
      if (!(full > 0.0f)) return r;
      const float x = (level(m, t, p, k) - noise) / full;
      if (x > max_pulse || (x > off_max && x < on_min)) return r;
      if (x >= on_min) {
        if ((modes::kModeAcEmptySlots >> k & 1u) != 0) return r;
        pulses |= 1u << k;
      }
      margin = std::min(margin, std::fabs(x - 0.5f));
    }
    // Gaps: what is left after the noise floor and the leakage of the
    // neighbouring pulses, as a share of the way from the noise floor to
    // the amplitude.
    const int last_gap = (pulses >> modes::kModeAcSpiSlot & 1u) ? modes::kModeAcSpiSlot : modes::kModeAcF2Slot;
    for (int g = 0; g <= kAcGaps; ++g) {
      if (g >= last_gap && g < kAcGaps) continue;  // after the reply's last pulse (another may start there)
      const Win& x = win[static_cast<std::size_t>(p * kAcWindows + kGapWin + g)];
      const bool prev = g < kAcGaps && (pulses >> g & 1u) != 0;  // window kAcGaps is the pre window: slot 0 follows it
      const bool after = g < kAcGaps ? (pulses >> (g + 1) & 1u) != 0 : true;
      const float leak = amp * ((prev ? x.leak_prev : 0.0f) + (after ? x.leak_next : 0.0f));
      if ((level(m, t, p, kGapWin + g) - noise - leak) * inv > gap_max) return r;
    }
    r.ok = true;
    r.score = l0 + l14;
    r.amp = amp;
    r.margin = margin;
    r.pulses = pulses;
    return r;
  }

  // The reply's template at start s (in samples relative to the pointer
  // later passed with it, kept small so that the arithmetic does not depend
  // on where the reply sits in the buffer): xs[i] is the share of sample
  // tj0 + i that the pulses cover (a sample of a rectangular pulse of
  // amplitude A has magnitude A times that), from 0.5 us before F1 to
  // 0.5 us after the reply's last pulse.
  std::ptrdiff_t tj0 = 0;
  std::size_t tn = 0;
  void fill_template(double s, std::uint32_t pulses, double spu) noexcept {
    const double last = (pulses >> modes::kModeAcSpiSlot & 1u) ? modes::kModeAcSpiSlot : modes::kModeAcF2Slot;
    tj0 = static_cast<std::ptrdiff_t>(std::floor(s - kResidualMarginUs * spu + 0.5));
    const auto j1 = static_cast<std::ptrdiff_t>(
        std::floor(s + (last * modes::kModeAcSlotUs + modes::kModeAcPulseUs + kResidualMarginUs) * spu + 0.5));
    tn = std::min(static_cast<std::size_t>(j1 - tj0 + 1), xs.size());
    std::fill(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(tn), 0.0);
    const double width = modes::kModeAcPulseUs * spu;
    for (int k = 0; k < kAcSlots; ++k) {
      if (!(pulses >> k & 1u)) continue;
      const double pa = s + k * modes::kModeAcSlotUs * spu, pb = pa + width;
      for (auto j = static_cast<std::ptrdiff_t>(std::floor(pa + 0.5)); static_cast<double>(j) - 0.5 < pb; ++j) {
        const double ov = std::min(pb, static_cast<double>(j) + 0.5) - std::max(pa, static_cast<double>(j) - 0.5);
        const std::ptrdiff_t i = j - tj0;
        if (ov > 0 && i >= 0 && static_cast<std::size_t>(i) < tn) xs[static_cast<std::size_t>(i)] += ov;
      }
    }
  }

  // Matched filter of the template: <m, x> / |x| (largest at the true start:
  // the samples are proportional to x there), and the least-squares
  // amplitude <m, x> / |x|^2.
  [[nodiscard]] double correlate(const float* m, double* amp) const noexcept {
    double sxy = 0.0, sxx = 0.0;
    for (std::size_t i = 0; i < tn; ++i) {
      sxy += xs[i] * static_cast<double>(m[tj0 + static_cast<std::ptrdiff_t>(i)]);
      sxx += xs[i] * xs[i];
    }
    if (!(sxx > 0)) {
      *amp = 0.0;
      return 0.0;
    }
    *amp = sxy / sxx;
    return sxy / std::sqrt(sxx);
  }

  // Step 6, on the template last filled: sample j's expected magnitude is
  // sqrt((A x_j)^2 + noise^2); noise alone leaves a squared residual of
  // about v_j per sample (Rayleigh: 0.273 noise^2 off the pulses, 0.637
  // noise^2 on them). False when the residual exceeds that by more than 4
  // of its standard deviations plus max_excess pulse energies: another
  // reply (or anything else) is in there too.
  [[nodiscard]] bool residual_ok(const float* m, double amp, float noise, double spu) const noexcept {
    const double mu2 = static_cast<double>(noise) * static_cast<double>(noise);
    double r = 0.0, v = 0.0, vv = 0.0;
    for (std::size_t i = 0; i < tn; ++i) {
      const double ax = amp * xs[i];
      const double d = static_cast<double>(m[tj0 + static_cast<std::ptrdiff_t>(i)]) - std::sqrt(ax * ax + mu2);
      r += d * d;
      const double vi = (xs[i] > 0 ? 0.637 : 0.273) * mu2;
      v += vi;
      vv += vi * vi;
    }
    return r - v <= 4.0 * std::sqrt(2.0 * vv) + max_excess * amp * amp * modes::kModeAcPulseUs * spu;
  }
};

void Demodulator::ModeAcDelete::operator()(ModeAc* p) const noexcept { delete p; }

void Demodulator::init_mode_ac() {
  ac_.reset(new ModeAc);
  ModeAc& a = *ac_;
  const double spu = spu_;
  auto add_window = [&](int p, int k, double from_us, double to_us) {
    const double lo = static_cast<double>(p) / kAcPhases + from_us * spu;
    const double hi = static_cast<double>(p) / kAcPhases + to_us * spu;
    ModeAc::Win& x = a.win[static_cast<std::size_t>(p * kAcWindows + k)];
    x.first = static_cast<std::int32_t>(std::floor(lo + 0.5));  // sample j covers [j - 0.5, j + 0.5)
    x.off = static_cast<std::uint32_t>(a.w.size());
    double sq = 0.0;
    for (std::int32_t j = x.first; static_cast<double>(j) - 0.5 < hi; ++j) {
      const double ov = std::max(0.0, std::min(hi, j + 0.5) - std::max(lo, j - 0.5));
      a.w.push_back(static_cast<float>(ov));
      sq += ov * ov;
    }
    x.count = static_cast<std::uint32_t>(a.w.size() - x.off);
    x.inv_width = static_cast<float>(1.0 / (hi - lo));
    x.gain = static_cast<float>(sq / (hi - lo));
  };
  // Level of window k of phase p for a unit pulse in slot `slot`.
  auto unit_level = [&](int p, int k, int slot) {
    const ModeAc::Win& x = a.win[static_cast<std::size_t>(p * kAcWindows + k)];
    const double pa = static_cast<double>(p) / kAcPhases + slot * modes::kModeAcSlotUs * spu;
    const double pb = pa + modes::kModeAcPulseUs * spu;
    double sum = 0.0;
    for (std::uint32_t i = 0; i < x.count; ++i) {
      const double j = static_cast<double>(x.first) + i;
      const double mag = std::max(0.0, std::min(pb, j + 0.5) - std::max(pa, j - 0.5));
      sum += static_cast<double>(a.w[x.off + i]) * mag;
    }
    return static_cast<float>(sum * static_cast<double>(x.inv_width));
  };
  for (int p = 0; p < kAcPhases; ++p) {
    for (int k = 0; k < kAcSlots; ++k) {
      add_window(p, k, k * modes::kModeAcSlotUs, k * modes::kModeAcSlotUs + modes::kModeAcPulseUs);
    }
    for (int g = 0; g < kAcGaps; ++g) {
      const double mid = g * modes::kModeAcSlotUs + 0.5 * (modes::kModeAcPulseUs + modes::kModeAcSlotUs);
      add_window(p, kGapWin + g, mid - kGapHalfUs, mid + kGapHalfUs);
    }
    add_window(p, kPreWin, kPreFromUs, kPreToUs);
    for (int g = 0; g < kAcGaps; ++g) {
      a.win[static_cast<std::size_t>(p * kAcWindows + kGapWin + g)].leak_prev = unit_level(p, kGapWin + g, g);
      a.win[static_cast<std::size_t>(p * kAcWindows + kGapWin + g)].leak_next = unit_level(p, kGapWin + g, g + 1);
    }
    a.win[static_cast<std::size_t>(p * kAcWindows + kPreWin)].leak_next = unit_level(p, kPreWin, 0);
  }
  // Samples read before a position: the pre window, and the timing climb's
  // reach. After it, `lag` (more than a reply, the climb and t + 1) keeps
  // every read inside the samples the Mode S scan has.
  a.back = static_cast<std::ptrdiff_t>(std::ceil((-kPreFromUs + kClimbSteps * kClimbStepUs) * spu)) + 4;
  a.f2_end = (modes::kModeAcF2Us + modes::kModeAcPulseUs) * spu;
  a.exclude_before = kExcludeBeforeUs * spu;
  a.exclude_after = (kReplyMaxUs + 0.5) * spu + 3.0;
  a.lag = static_cast<std::ptrdiff_t>(std::ceil(a.exclude_after + a.exclude_before)) + 8;
  // Echo suppression (step 6) can move a Mode S frame up to first_arrival_us
  // earlier than where the Mode S scan found it: replies wait that much longer,
  // so a retimed frame never lands before one already emitted.
  if (cfg_.first_arrival_us > 0) a.lag += static_cast<std::ptrdiff_t>(std::ceil((cfg_.first_arrival_us + 1.0) * spu)) + 4;
  // Gate: of the two windows of n samples starting at the sample that holds
  // a pulse's start and at the one after it, the better holds at least
  // max(floor(w), w / 2) samples of the pulse (w samples long) for
  // n = floor(w) + 1, and all of it for n = ceil(w) + 1. The width whose
  // threshold stands further above the noise's window sum (in standard
  // deviations of it, Rayleigh: 0.52 of the mean per sample) is used.
  const double wpulse = modes::kModeAcPulseUs * spu;
  double best_z = -1e300;
  for (const double n : {std::floor(wpulse) + 1.0, std::ceil(wpulse) + 1.0}) {
    const double covered = n > std::floor(wpulse) + 1.0 ? wpulse : std::max(std::floor(wpulse), 0.5 * wpulse);
    const double thr = kGateShare * (cfg_.ac_min_signal_ratio * covered + (n - covered));
    const double z = (thr - n) / (0.52 * std::sqrt(n));
    if (z > best_z) {
      best_z = z;
      a.gwin = static_cast<std::ptrdiff_t>(n);
      a.gate_scale = static_cast<float>(thr);
    }
  }
  a.g14 = static_cast<std::ptrdiff_t>(std::floor(modes::kModeAcF2Us * spu + 0.5));
  a.g7 = static_cast<std::ptrdiff_t>(std::floor(modes::kModeAcXSlot * modes::kModeAcSlotUs * spu + 0.5));
  a.g15 = static_cast<std::ptrdiff_t>(std::floor(15 * modes::kModeAcSlotUs * spu + 0.5));
  a.min_ratio = static_cast<float>(cfg_.ac_min_signal_ratio);
  a.framing_ratio = static_cast<float>(cfg_.ac_framing_ratio);
  a.off_max = static_cast<float>(0.5 - cfg_.ac_decision_band);
  a.on_min = static_cast<float>(0.5 + cfg_.ac_decision_band);
  a.max_pulse = static_cast<float>(cfg_.ac_max_pulse);
  a.gap_max = static_cast<float>(cfg_.ac_max_gap);
  a.max_excess = cfg_.ac_max_residual;
  a.xs.assign(static_cast<std::size_t>(std::ceil((kReplyMaxUs + 2 * kResidualMarginUs) * spu)) + 8, 0.0);

  // Mode A/C keeps up to `lag + back` more samples than the Mode S scan:
  // room for them, and for the replies and held frames.
  const std::size_t extra = static_cast<std::size_t>(a.lag + a.back) + 64;
  buf_.resize(buf_.size() + extra, 0.0f);
  a.ws.assign(buf_.size(), 0.0f);
  a.gate.assign(buf_.size(), 0);
  const std::size_t replies = static_cast<std::size_t>(static_cast<double>(buf_.size()) / std::max(1.0, a.f2_end)) + 16;
  const std::size_t modes_cap = out_.size() + 64;
  a.pending.resize(modes_cap + replies);
  a.modes.resize(modes_cap + replies);
  a.found.resize(replies);
  out_.resize(modes_cap + 2 * replies);
  // The noise ring must hold every block the larger buffer can (slots are
  // keyed by block number, so the Mode S floor is the same).
  block_est_.assign(block_est_.size() + extra / kNoiseBlock + 2, 0.0f);
  block_id_.assign(block_est_.size(), ~std::uint64_t{0});
}

std::ptrdiff_t Demodulator::scan_mode_ac(bool final, std::ptrdiff_t end) {
  ModeAc& a = *ac_;
  const float* m = buf_.data();
  const auto valid = static_cast<std::ptrdiff_t>(valid_);
  if (a.next < base_) a.next = base_;  // after a skip()
  const std::ptrdiff_t from = std::max(static_cast<std::ptrdiff_t>(a.next - base_), a.back);
  const std::ptrdiff_t to = final ? valid : end - a.lag;

  // The Mode S frames that may overlap: those held from the last call, then
  // this call's (both in time order, the held ones first).
  std::size_t n_modes = 0;
  for (std::size_t i = 0; i < a.n_pending; ++i) a.modes[n_modes++] = a.pending[i];
  for (std::size_t i = 0; i < n_out_; ++i) a.modes[n_modes++] = out_[i];
  std::size_t n_found = 0;

  std::ptrdiff_t u = from;
  if (to > from) {
    // Step 1.
    float* ws = a.ws.data();
    const std::ptrdiff_t w_end = to + a.g15 + 2;
    if (a.gwin == 2) {  // 2.2-4.4 MS/s: a fixed width the compiler vectorizes
      for (std::ptrdiff_t k = from; k < w_end; ++k) ws[k] = m[k] + m[k + 1];
    } else {
      for (std::ptrdiff_t k = from; k < w_end; ++k) {
        float acc = 0.0f;
        for (std::ptrdiff_t j = 0; j < a.gwin; ++j) acc += m[k + j];
        ws[k] = acc;
      }
    }
    for (std::ptrdiff_t k = from; k + 1 < w_end; ++k) ws[k] = std::max(ws[k], ws[k + 1]);
    std::uint8_t* g = a.gate.data();
    const std::ptrdiff_t g14 = a.g14, g7 = a.g7, g15 = a.g15;
    for (std::ptrdiff_t seg = from; seg < to;) {
      const std::uint64_t blk = (base_ + static_cast<std::uint64_t>(seg)) / kNoiseBlock;
      const auto seg_end = std::min<std::ptrdiff_t>(to, static_cast<std::ptrdiff_t>((blk + 1) * kNoiseBlock - base_));
      const float thr = a.gate_scale * noise_for(blk, final);
      for (std::ptrdiff_t k = seg; k < seg_end; ++k) {
        // X and slot 15 empty: their windows below kGateXShare of the
        // framing windows' mean.
        const float empty = kGateXShare * 0.5f * (ws[k] + ws[k + g14]);
        g[k - from] = static_cast<std::uint8_t>((ws[k] > thr) & (ws[k + g14] > thr) & (ws[k + g7] < empty) &
                                                (ws[k + g15] < empty));
      }
      seg = seg_end;
    }

    std::size_t mi = 0;  // Mode S frames starting before the current reply could end
    double max_end = a.modes_end;
    std::uint64_t noise_blk = ~std::uint64_t{0};
    float noise = 0.0f;
    for (; u < to; ++u) {
      if (!g[u - from]) {
        const void* hit = std::memchr(g + (u - from), 1, static_cast<std::size_t>(to - u));
        if (hit == nullptr) {
          u = to;
          break;
        }
        u = from + (static_cast<const std::uint8_t*>(hit) - g);
      }
      ++stats_.ac_gated;
      // Step 2.
      const double q = static_cast<double>(base_ + static_cast<std::uint64_t>(u));
      // A retimed frame (echo suppression) also covers its echo copy, echo_delay_us later.
      while (mi < n_modes && a.modes[mi].sample_pos < q + a.exclude_after) {
        const DemodFrame& f = a.modes[mi++];
        if (f.len != 2) max_end = std::max(max_end, f.sample_pos + (8.0 + 8.0 * f.len + static_cast<double>(f.echo_delay_us)) * spu_);
      }
      if (max_end > q - a.exclude_before) {
        // Jump to the first position the frame no longer overlaps.
        ++stats_.ac_in_mode_s;
        const double clear = max_end + a.exclude_before - static_cast<double>(base_);
        u = std::max(u, static_cast<std::ptrdiff_t>(std::ceil(clear)) - 1);
        continue;
      }
      if (const std::uint64_t blk = (base_ + static_cast<std::uint64_t>(u)) / kNoiseBlock; blk != noise_blk) {
        noise_blk = blk;
        noise = noise_for(blk, final);
      }
      // Steps 3-4 at u, and (once something passes) at u + 1.
      AcCandidate best;
      std::ptrdiff_t best_t = 0;
      int best_p = 0;
      bool framing = false;
      for (std::ptrdiff_t tt = u; tt < u + 2; ++tt) {
        if (tt > u && !best.ok) break;
        for (int p = 0; p < kAcPhases; ++p) {
          const AcCandidate c = a.evaluate(m, tt, p, noise);
          framing = framing || c.framing;
          if (c.ok && (!best.ok || c.score > best.score)) {
            best = c;
            best_t = tt;
            best_p = p;
          }
        }
      }
      if (framing) ++stats_.ac_framing;
      if (!best.ok) {
        if (framing) ++stats_.ac_garbled;
        continue;
      }
      // Step 5: timing. Hill climb on the template's matched filter in
      // 0.04 us steps from the winning phase, then a parabola.
      // Positions relative to best_t.
      const double step = kClimbStepUs * spu_;
      const double s0 = static_cast<double>(best_p) / kAcPhases;
      const float* mt = m + best_t;
      double amp_c = 0.0, amp_side = 0.0;
      auto score_at = [&](double at, double* amp) {
        a.fill_template(at, best.pulses, spu_);
        return a.correlate(mt, amp);
      };
      int jc = 0;
      double yc = score_at(s0, &amp_c);
      double ym = score_at(s0 - step, &amp_side);
      double yp = score_at(s0 + step, &amp_side);
      const int dir = yp > yc ? 1 : (ym > yc ? -1 : 0);
      while (dir != 0 && std::abs(jc) < kClimbSteps) {
        const double ahead = dir > 0 ? yp : ym;
        if (!(ahead > yc)) break;
        jc += dir;
        if (dir > 0) {
          ym = yc;
          yc = yp;
          yp = score_at(s0 + (jc + 1) * step, &amp_side);
        } else {
          yp = yc;
          yc = ym;
          ym = score_at(s0 + (jc - 1) * step, &amp_side);
        }
      }
      double s_best = s0 + jc * step;
      {
        const double den = ym - 2 * yc + yp;
        if (den < 0 && yc >= ym && yc >= yp) s_best += step * std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
      }
      // Step 6.
      double fit_amp = 0.0;
      score_at(s_best, &fit_amp);
      if (!(fit_amp > 0.0) || !a.residual_ok(mt, fit_amp, noise, spu_)) {
        ++stats_.ac_garbled;
        ++stats_.ac_residual;
        u = std::max(u, best_t);
        continue;
      }
      if (n_found == a.found.size()) break;  // cannot happen: sized for the buffer
      DemodFrame& f = a.found[n_found++];
      f = DemodFrame{};
      const std::uint16_t code = modes::mode_ac_code(best.pulses);
      f.bytes[0] = static_cast<std::uint8_t>(code >> 8);
      f.bytes[1] = static_cast<std::uint8_t>(code & 0xFF);
      f.len = 2;
      f.df = 32;
      f.sample_pos = static_cast<double>(base_ + static_cast<std::uint64_t>(best_t)) + s_best;
      f.signal = static_cast<float>(fit_amp);
      f.noise = noise;
      f.snr_db = noise > 0 ? 20.0f * std::log10(f.signal / noise) : 99.0f;
      f.margin = best.margin;
      ++stats_.ac_frames;
      // Resume after F2 (a reply starting inside this one garbles both).
      u = std::max(u, best_t + static_cast<std::ptrdiff_t>(std::floor(s_best + a.f2_end)));
    }
  }
  a.next = base_ + static_cast<std::uint64_t>(std::max(u, from));

  // Merge both kinds in time order; what lies past the merge point waits.
  const double limit = final ? std::numeric_limits<double>::infinity()
                             : static_cast<double>(base_ + static_cast<std::uint64_t>(std::max(to, from))) - 1.0;
  n_out_ = 0;
  a.n_pending = 0;
  std::size_t i = 0, j = 0;
  while (i < n_modes || j < n_found) {
    const bool take_modes = j == n_found || (i < n_modes && a.modes[i].sample_pos <= a.found[j].sample_pos);
    const DemodFrame& f = take_modes ? a.modes[i++] : a.found[j++];
    if (f.sample_pos < limit) {
      out_[n_out_++] = f;
      if (f.len != 2) a.modes_end = std::max(a.modes_end, f.sample_pos + (8.0 + 8.0 * f.len + static_cast<double>(f.echo_delay_us)) * spu_);
    } else {
      a.pending[a.n_pending++] = f;
    }
  }
  return std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(a.next - base_) - a.back);
}

}  // namespace adsb::demod
