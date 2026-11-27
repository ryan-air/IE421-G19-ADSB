# MATLAB ADS-B receiver comparison

Use the C++ signal generator to produce one file per scenario. Each receiver
sees the same samples, and the existing SigMF scorer supplies the ground truth.
The MATLAB runner depends on a receiver callback, so it can also drive a
different MATLAB or Simulink receiver without changing generation or scoring.

MathWorks provides a [Communications Toolbox ADS-B receiver example](https://www.mathworks.com/help/comm/ug/airplane-tracking-using-ads-b-signals.html)
and a [packet-based ADS-B transceiver example](https://www.mathworks.com/help/soc/ug/ADSB.html).
The receiver example accepts captured complex baseband and provides raw bits,
CRC status and reception time. The transceiver example shows packet generation
on SDR hardware. This harness uses the project's scenario generator for
multi-aircraft motion, propagation, overlapping replies and per-message truth.

## First comparison

From the repository root, after building the release preset:

```sh
mkdir -p core/build/matlab-comparison
core/build/release/tools/adsb-iqsynth -o core/build/matlab-comparison/busy \
  --aircraft 150 --duration 10 --seed 421 --rate 2400000 --format cu8 --tail 1
core/build/release/tools/adsb-demod core/build/matlab-comparison/busy.sigmf-meta \
  --beast core/build/matlab-comparison/busy.ours.beast
```

Open the MathWorks receiver example from the documentation so MATLAB has local
copies of its helpers and `defaultinputsADSB.mat`. In R2025b, use
`openExample('comm/AirplaneTrackingUsingADSBSignalsExample')`. From the
example directory, run the following with an absolute path to the repository:

```matlab
repo = '/absolute/path/to/IE421-G19-ADSB';
addpath(fullfile(repo, 'bench', 'matlab'));
receiver = mathworksReceiver(pwd);
base = fullfile(repo, 'core', 'build', 'matlab-comparison', 'busy');
count = decodeSigmf([base '.sigmf-meta'], [base '.matlab.beast'], receiver)
```

The adapter uses the example's captured-signal configuration and replaces its
sample source with the SigMF file. It takes CRC-passing packets from the PHY,
before aircraft parsing and display. The settings helper may download the
example recording during initialization. The adapter deliberately uses the
example's 2.4 MS/s input rate and lets its PHY do the interpolation. Use
`testDecodeSigmf` to exercise file I/O and packet export with a fake receiver
without the toolbox.

Score just DF17 first, using the same format scope for both receivers:

```sh
cd core/build/matlab-comparison
python3 ../../scripts/iq-score.py busy.sigmf-meta busy.ours.beast --df 17 --json
python3 ../../scripts/iq-score.py busy.sigmf-meta busy.matlab.beast --df 17 --json
```

Compare counts by clean/collided and SNR band, unmatched detections, and
timestamp error after aligning each decoder's constant offset. Record the
MATLAB release and example revision used. For a third implementation, capture
pinned readsb's Beast output using the file-input settings in
[`check-demod.sh`](../../core/scripts/check-demod.sh) and score it with the same
DF scope.

## Scenario matrix

Start from the same seed and change one factor per run. Reuse the same
recording across all receivers. The generator varies position and altitude
across aircraft. Its atmospheric model is effective-Earth-radius refraction
and radio-horizon loss.

| Scenario | Generator flags relative to the pilot | Compare |
|---|---|---|
| Busy / overlap | `--aircraft 600` | Recall for clean and overlapped replies |
| Weak | `--noise-figure 22` | Recall across SNR bands |
| Multipath | `--ground-reflection 0.7 --reflector 40.06,-88.25,250,6` | Echo copies and timing |
| Radio horizon | `--max-range 200 --k-factor 1`, then `--k-factor 1.333` with the same range | Decode behavior under different refraction assumptions |
| Doppler control | Compare defaults with `--no-doppler` | Decode agreement with and without geometry-derived Doppler |
| Frequency estimator | Vary `--tx-offset-sd`, `--tx-drift-sd`, `--tx-jitter-sd`, and receiver `--lo-drift` | Estimation error against known offsets and drift using `adsb-carrier` |

The frequency row exercises the method, including Doppler and receiver-error
removal. Estimating stability by aircraft type requires real IQ and aircraft
type metadata. See the [carrier study procedure](../../REPRODUCING.md#transponder-carrier-frequency-study-method).

## Measured comparison

MATLAB R2025b Update 7, Communications Toolbox 25.2, and `adsb-demod` from
`af48290`. The pilot used 20 aircraft and 3 seconds of traffic. The remaining
cases used 6 seconds of traffic, 150 aircraft unless specified, and the flags
in the scenario matrix. Every case used seed 421, 2.4 MS/s `cu8`, and a
1-second tail. The table counts matched DF17 messages out of the rendered
truth, including weak and overlapping frames.

| Scenario | DF17 truth | adsb-demod matched | MathWorks matched |
|---|---:|---:|---:|
| Pilot | 234 | 211 | 195 |
| Baseline | 3,417 | 2,746 | 2,571 |
| Weak | 3,316 | 373 | 197 |
| Multipath | 3,417 | 1,801 | 1,526 |
| Horizon, k = 1 | 2,635 | 1,995 | 1,810 |
| Horizon, k = 1.333 | 2,940 | 2,358 | 2,064 |
| Doppler disabled | 3,417 | 2,748 | 2,570 |
| Busy, 600 aircraft | 13,615 | 7,614 | 6,564 with expanded capacity |

The stock MathWorks example stopped on the busy case when the candidate count
exceeded its 45-slot buffer. For the busy value in the table, construct the
adapter with `mathworksReceiver(exampleDir, 180)`. This changes the candidate
buffer capacity while leaving the example's detection logic and frame size
in place. Other rows use the default adapter.

The largest absolute MathWorks timestamp residual after fitting a constant
offset was 0.36 samples or less outside the multipath case, where it reached
2.60 samples. In the multipath case the scorer counted 10 unmatched DF17
outputs from `adsb-demod` and two from MathWorks. These are synthetic waveform
results on one seed. The horizon settings change which frames are rendered,
so their recall denominators differ.

## Where MATLAB loses DF17 messages

`mathworksDiagnosticReceiver` calls the same interpolation, synchronization
and bit parser helpers as the stock example and logs every accepted preamble
with its candidate time, parsed format, payload and CRC status. It reports
candidate overflow explicitly. Keep one adapter per recording and clear it
after decoding to close the log.

```matlab
diagnostic = mathworksDiagnosticReceiver(pwd, [base '.candidates.csv']);
decodeSigmf([base '.sigmf-meta'], [base '.diagnostic.beast'], diagnostic);
clear diagnostic
```

From the repository root, replace `BASE` with the same capture path:

```sh
python3 core/scripts/iq-score.py BASE.sigmf-meta BASE.diagnostic.beast --df 17 --json > BASE.score.json
python3 bench/matlab/scoreCandidates.py BASE.sigmf-meta BASE.score.json BASE.candidates.csv
```

The scorer aligns the receiver clock using the Beast score. It pairs each
truth message with at most one candidate within four input samples
(1.67 µs at 2.4 MS/s), preferring exact, CRC-passing payloads. It reports
success, CRC rejection, unsupported parsed format, a different CRC-passing
payload, or no on-time preamble. This last bucket includes candidates whose
timing falls outside the window, including echoes. A time-only association
can also belong to a different overlapping transmission. Treat these as
diagnostic categories rather than proof of the underlying failure mechanism.

### Diagnostic results

MATLAB R2025b Update 7, seeds 421, 422 and 423, 150 aircraft, 6 seconds
plus a 1-second tail per run, 2.4 MS/s `cu8`. The scenario flags match the
weak and multipath rows above. The diagnostic adapter's Beast output was
byte-for-byte identical to the stock receiver's in all six runs.

| Scenario | DF17 truth | Matched | No on-time preamble | CRC rejected | Unsupported parsed format |
|---|---:|---:|---:|---:|---:|
| Weak | 10,122 | 709 | 8,677 | 589 | 147 |
| Multipath | 10,425 | 4,855 | 3,419 | 1,994 | 157 |

No truth message was assigned a different CRC-passing payload. The weak
scenarios put 92.2% of misses in the preamble/timing bucket. This still
accounts for 361 of 421 misses among weak-scenario frames at 15 dB or higher.
In multipath, 38.6% of misses had an on-time candidate rejected by the parser.
For weak signals, the next experiment should instrument peak selection and
preamble validation separately. For multipath, test alternate sampling
phases on failed candidates and track unmatched output and late copies.
