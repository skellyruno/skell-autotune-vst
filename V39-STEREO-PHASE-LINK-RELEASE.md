# SkellyTune v39 — Linked stereo predictive-grain marks

## Root cause and correction

SkellyTune v38 and the initial v39 synthesis candidate used *independent* waveform peak refinement in the left and right predictive shifters. Near-dual-mono files with very small floating-point differences sometimes selected different neighboring crests. A pitch grain can have the correct average fundamental frequency and still interfere destructively with the other channel when summed to mono. This defect is independent of musical target-note decisions.

The Low Latency shifters now share an **audio-thread-local, fixed-capacity phase-mark timeline**. Channel 0 determines the source mark position and writes it into a small, preallocated ring indexed by grain sequence number. Channel 1 consumes that position, but reads and synthesizes **its own audio waveform**. No output channel is duplicated, no future samples are read, and no locks or heap allocation were added to the callback. The link is enabled only in experimental Low Latency mode; Standard processing is intentionally unchanged.

Each shifter resets its grain sequence on range/mode changes. The leader writes before the follower in the existing per-segment processing sequence, avoiding stale mark use. A dedicated regression exercises Low Latency ON/OFF, true stereo, and sample-accurate range/mode changes with different host blocks.

## Measured effect on the supplied vocals

At 44.1 kHz, maximum retune, Formant Preserve OFF, using the same original G-major and A-major audio and documented range settings:

| Metric | Prior v39 | Linked marks |
|---|---:|---:|
| G-major L/R correlation, Low Latency ON | 0.5802 | 1.0000 |
| A-major L/R correlation, Low Latency ON | 0.9723 | 1.0000 |
| G-major active 200 ms windows >1 dB lower on mono fold than left channel | 32 / 113 | 0 / 113 |
| G-major active windows >3 dB lower on mono fold | 13 / 113 | 0 / 113 |
| Worst G-major mono-fold loss relative to left | -7.62 dB | 0 dB |
| A-major windows >1 dB lower on mono fold | 2 / 117 | 0 / 117 |
| G-major mono-fold sustained-note p95 pitch error | 9.66 cents | 8.95 cents |
| A-major mono-fold sustained-note p95 pitch error | 14.53 cents | 15.23 cents |

**Trade-off:** Some pitch statistics on folded audio change, because removing phase cancellation changes the waveform seen by the external pitch analyzer. A-major p95 increases modestly. The Low Latency **left-channel waveform remains sample-identical** to the prior v39 candidate for both test vocals, so this patch does not alter the baseline musical pitch controller or its left-channel synthesis. Both Standard-mode stereo WAVs are sample-identical to the previous version. The nominal 84-sample Low Latency budget is unchanged; detector decision latency is additional.

The pitch figures use the supplied commercial-consensus masks. They are not evidence of subjective parity with Auto-Tune Pro or XPitch. They are distinct from the correlation and fold-level correctness checks.

## Reproducible regression

`tests/fixtures/near_dual_mono_4s.f32` is a 4-second, interleaved 32-bit float excerpt of the user-provided G-major raw vocal. The new `stereo_phase_mark_link_regression` **fails on the unmodified previous source** due to near-dual-mono channel dephasing (correlation 0.406 on this fixture) and passes on v39 (correlation ~1.000). The test also verifies true stereo differences are *retained* and that block size and mode/range automation are sample consistent.

Build the offline DSP suite:

```bash
cmake -S . -B build -DSKELLYTUNE_DSP_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j 8
ctest --test-dir build -C Release --output-on-failure
```

Measure local stereo fold losses:

```bash
python scripts/measure-stereo-phase.py RealVocal-v39/v39-LowLatency-Gmajor.wav RealVocal-v39/v39-Standard-Gmajor.wav
```

The package includes the new A/G Standard and Low Latency 32-bit WAV renders, CSV traces, frozen reference masks, and the complete C++ source. The old G-major first-four-seconds reproduction and the fixed equivalent are also included for instant listening.

## Verification and remaining limits

- GCC Release: 28/28 offline DSP tests passed.
- Clang Release: 28/28 offline DSP tests passed.
- The **new stereo regression** passed with AddressSanitizer and UBSan, including the automation case.
- The broader sanitizer compilation did not finish within the execution limit. Full sanitizer suite is **not** claimed as passed.
- 127 and 512-sample host-buffer outputs are bit-identical for the 4-second regression, including sample-accurate range and latency-mode changes.
- Full native JUCE/VST3 build, FL Studio behavior, and subjective listening **have not been verified in this environment**.
- The unresolved Auto-Tune Pro/XPitch gap remains in high-percentile note accuracy, perceptual artifacts during transitions, and occasional above-0 dBFS float peaks in Standard mode.
