# SkellyTune → Codex engineering handoff (19 October 2026)

**Goal:** Close the measurable and audible gap to Auto-Tune Pro and XPitch without compromising real-time safety, session compatibility, or a known-good latency mode. This is a **continuation** of the v39 codebase, not a new plugin rewrite unless carefully justified by controlled A/B and regression evidence.

## Project and present status

- `Source/AutoTuneEngine.h`: causal pitch controller, scale targeting, detector confidence/voicing, dry handoff, stereo analysis, glue to shifters, mode/range automation and reported latency.
- `Source/PitchDetector.h`: decimated, anti-aliased YIN/CMND fundamental estimation.
- `Source/PitchShifter.h`: Standard and experimental Low Latency causal predictive overlap-add/phase matching, grain normalization, stereo pitch-mark link. The Low Latency correction still produces transient artifacts and note-boundary pitch-tail errors.
- `Source/SteadyNoteForecast.h`, `NoteTransition.h`, `NeutralBypassGate.h`, `TargetGlide.h`, `ScaleQuantizer.h`, `KeyEstimator.h`: supporting control paths. Inspect their actual APIs, not prior prose alone.
- `Source/PluginProcessor.cpp` and `PluginEditor.cpp`: JUCE VST3, parameter APVTS, editor. The full plugin **has not been compiled or hosted** in this environment.
- `tests/`: DSP-only regression executables and stereo/automation fixtures; `tests/JuceHeader.h` is only an offline shim.
- `RealVocal-v39/`: frozen G/A held-frame masks, four processed float WAV renders, per-hop traces, stereo fold metrics, 4-second before/after samples.
- `Benchmarks/Input/`: optionally extracted user-provided commercial reference and raw audio pack, not suitable for public distribution without consent.
- `scripts/compare-vocal-cohort.py`: general cohort comparator (onset-envelope alignment, not host latency); `scripts/codex-benchmark.py`: one-command G/A comparison once the separate audio pack is extracted.

### Confirmed v39 results (offline, not perceptual parity)

- Near-dual-mono Low Latency source-mark linkage fixed destructive stereo cancellation: G major channel correlation approximately **0.580 → 1.000**, A major **0.972 → 1.000**.
- Mono fold relative to the processed left channel: G major **32/113 windows below -1 dB → 0/113**, worst **-7.62 dB → 0.00 dB**. True stereo input remains distinct.
- Frozen G-major stable consensus mask (605 frames): v39 Low Latency mono-fold **median 1.979 cents; p95 8.947 cents**, v39 Standard **median 1.499 cents; p95 10.765 cents**. These are *selective* held-note frames, not fast onset or full-vocal quality scores.
- A-major mask (1,337 frames): v39 Low Latency folded **median 2.335 cents; p95 15.227 cents**; Standard **median 1.661 cents; p95 12.611 cents**. Linking fixes fold cancellation but **A-major p95 rose slightly** relative to previously unlinked fold.
- Competition reference examples on same G-major frozen mask, from the v38 report: Auto-Tune Pro Low Male p95 **4.425 cents**, XPitch Low Latency **4.448**, XPitch Real-Time **4.544**, XPitch HQ **5.026**, Waves Tune **10.534**. Recompute from audio for any new masks. Do not compare unlike masks or different reference configurations.
- The v39 project originally reported **28/28 GCC and Clang Release** offline tests; the *new stereo regression* passed ASan/UBSan. The **full sanitizer suite and native JUCE VST3/FL Studio testing remain unverified** in the original report.
- For this handoff, I independently configured and compiled the DSP-only project and reran **28/28 CTest cases successfully** in GCC Release. No full VST3 or DAW test was performed during handoff preparation.


### Fresh broader-mask cohort benchmark run during handoff

The added `scripts/codex-benchmark.py` was **actually executed** with the separate audio pack, not merely syntax-checked. Results are copied into `Benchmarks/Baseline-v39/`. They use **different** reference-consensus masks and envelope alignment from the historic 605/1,337-frame benchmark, so the figures must not be compared as the same measurement.

| Fresh cohort | Stable consensus frames | v39 Standard median / p95 | v39 Low Latency median / p95 | Leading commercial reference p95 |
|---|---:|---:|---:|---:|
| G major (five Auto-Tune/XPitch references) | 1,548 | 1.966 / 17.693 cents | 2.608 / 20.606 cents | Auto-Tune Low Male 9.174; XPitch HQ 9.153 cents |
| A major (Auto-Tune + Waves, MP3-derived WAVs) | 2,405 | 1.625 / 11.256 cents | 2.246 / 14.799 cents | Auto-Tune Pro 6.477 cents |

The G-mask now includes fast/difficult notes that are excluded in the legacy narrow test. Importantly, this exposes a **larger high-percentile gap**; it is a more useful go/no-go gate for future candidates. `alignment.csv` contains estimated onset-envelope lags; these are **not** VST3 transport latency or pitch-decision latency. A-major source MP3 coding and changing masks affect precision.

### Precisely reproduce the settings

- G major: root pitch class **7**, scale type **0 (Major)**, Retune Speed **0 (maximum)**, amount 1, Humanize/Flex/Target Glide 0, Note Stabilizer OFF, Formant Preserve OFF; Voice Range index **4** (Fastest 180 Hz) with **Catch Low Notes ON**, which effectively tracks down to **80 Hz**. **Standard transport latency 726 samples**, Low Latency experimental budget **84 samples** at 44.1 kHz.
- A major: root pitch class **9**, scale type 0, Retune 0, Formants OFF; Voice Range index **2 (100 Hz)**; **Standard 582 samples**, experimental Low Latency **84 samples**, at 44.1 kHz.
- These are **transport/sample-delay budgets** only. Detector observation and audible note commitment may be later. Test VST3 host getLatencySamples()/PDC independently.
- A saved plugin instance's host wrapper mixing, bypass, automation and defaults may differ from the offline harness; verify the entire APVTS→engine path and state recall.

## Repro commands

Offline regression (Linux/macOS):

```bash
cmake -S . -B build-dsp -DSKELLYTUNE_DSP_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp --config Release --parallel 6
ctest --test-dir build-dsp -C Release --output-on-failure -j 6
```

Windows VST3 (requires JUCE 8.0.4 FetchContent network access and Visual Studio toolchain):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Output target: `build/HumHouseVocalTune_artefacts/Release/VST3/SkellyTune.vst3` and Standalone companion. The GitHub Actions workflow also packages them. **Build alone does not prove FL Studio stability, preset recall, or sound quality.**

Fast regression:

```bash
ctest --test-dir build-dsp -C Release -R 'stereo_phase_mark_link_regression|v39_steady_phase_precision_regression|range_protection_regression|dry_delay_automation_regression' --output-on-failure
```

Cohort benchmark **after extracting `SkellyTune-Codex-Benchmark-Inputs.zip` directly into this project root**:

```bash
python -m pip install numpy scipy soundfile librosa
python scripts/codex-benchmark.py --key both
```

Outputs: `Benchmarks/Results/G/summary.csv`, `Benchmarks/Results/A/summary.csv`. This runs a **fresh mask** defined by the supplied commercial references, which may differ from `RealVocal-v39/*.csv`. Keep masks and counts explicit. `scripts/benchmark-v39.py` in the original archive references older `RealVocal-v34`, `v36`, `v38` directories *not packaged in this v39 ZIP*; **do not use it unmodified and claim it is self-contained**. Use the new wrapper or supply the missing historic files.

Original full-vocal lossless G references: Auto-Tune Pro Low Male/Soprano, XPitch HQ/Real-Time/Low Latency, Waves Tune Real-Time, FL Studio Pitcher, plus unprocessed vocal. A references: unprocessed vocal, Auto-Tune Pro, Waves Tune (originally supplied as MP3, decoded to float WAV in benchmark pack; keep compression caveat). They are user-provided *renders*, not models/binaries/algorithms.

## Highest-priority engineering questions

1. Are the remaining G/A difficult-note errors caused by wrong detector F0, wrong committed note, target timing, or the synthesized F0 failing an already-correct note? Add a single aligned per-hop trace with raw F0, confidence, onset strength, committed MIDI note, forecasted input pitch, target F0, output ratio, pitch-mark position and correction, contributor count, gain, and output step; keep tracing offline-only or opt-in outside RT callback.
2. During 100–300 cent jumps, do adjacent grains lose source phase/energy when a new target clock is committed? Implement a causal candidate phase-continuity strategy, with a short-transition synthetic regression and both-vocal listening renders. The existing 84-sample experimental transport constraint is strict unless a higher-latency option is explicit and correctly host-reported.
3. Are the remaining unvoiced ↔ voiced handoffs and near-unity bypass switches generating perceptible transient roughness? Compare local 20–100 ms envelope loss/click proxies and listening snippets, not median F0 alone.
4. Does Standard peak beyond +0 dBFS only during genuine resynthesis overlap or because of export gain? Analyze pre-peak windows, retain musical envelopes. Avoid a blind clipper/limiter.
5. Native plugin: verify JUCE 8.0.4 build, FL Studio load/close/session recall, host parameter automation, getLatencySamples/PDC after mode/range changes, and buffer/sample-rate/multichannel behavior. Report these separately from offline tests.

## Delivery contract

Implement substantial, contained code improvements in incremental commits, run the full suite, benchmark at least G/A and two buffer sizes, and provide two updated G/A stereo WAVs for each mode, a CSV/plot and an engineering report. Include rejected experiments and negative metrics. The goal is to reduce the gap to the strongest Auto-Tune Pro/XPitch references, **not** to declare parity by matching a selective median score. If no change beats baseline without new defects, report what failed and ship tested diagnostics instead.
