# Codex repository instructions — SkellyTune v39

You are maintaining a production-intended JUCE C++20 VST3 vocal pitch corrector. Read `CODEX_HANDOFF.md`, `CODEX_TASKS.md`, `V39-STEREO-PHASE-LINK-RELEASE.md`, and `CMakeLists.txt` **before** edits. Treat v39 as a tested offline DSP baseline, not a certified DAW plugin.

## Start here

```bash
cmake -S . -B build-dsp -DSKELLYTUNE_DSP_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp --config Release --parallel 6
ctest --test-dir build-dsp -C Release --output-on-failure -j 6
```

The baseline has 28 CTest targets and Release test assertions must stay enabled. On a Windows system with JUCE download access, run `cmake -S . -B build -A x64 && cmake --build build --config Release --parallel`, then test the VST3/Standalone and report the host results separately. **Do not claim a VST3 or FL Studio pass from the offline tests.**

## Code quality and safety rules

- Maintain C++20, JUCE integration, existing APVTS parameter IDs, preset/session recall, scale definitions, and VST3 delay reporting. Never silently change default modes or break stored projects.
- Keep the audio callback bounded: no heap allocation, locks, blocking I/O, logging, or large buffer clearing. Heavy analysis, plotting, filesystem work, and CSV writing belong offline or outside the audio callback.
- No noncausal future-input reads; explicitly distinguish reported sample latency from detector observation time, note commit time, and perceptual pitch settling.
- Do not fix quiet phrases by blindly applying broadband gain, mixing uncorrected pitched vowels, or adding a limiter unless a separate measured, tested case demonstrates the change is required.
- Keep genuinely stereo signals independent while maintaining phase-mark linkage for near-dual-mono Low Latency material. Regression: `tests/stereo_phase_mark_link_regression.cpp`; never drop the fixture.
- Reproduce a failure before changing code; make focused, revertible edits; reject candidates that make p95/p99, vocal energy, onset quality, block-size determinism, or CPU worse without explicitly documenting the tradeoff.
- Benchmark **both G major and A major**, Low Latency ON/OFF, and reference **Auto-Tune Pro Low Male + Soprano, XPitch HQ + Real-Time + Low Latency, Waves Tune**, where source renders exist. Some artifacts are held-note pitch proxies, not perceptual quality ground truth.
- User-provided commercial *rendered audio* is for comparison only. No protected binary DSP extraction, copying proprietary source, or bypassing licensing.
- Clearly label: confirmed regression pass, analytic measurement, preliminary listening impression, host validation, and unknown. Never fabricate test results.

## Done definition for each PR

1. State exactly which audible problem was reproduced (timestamps/inputs) and the measured root cause.
2. Keep all existing 28 tests green and add a regression that fails on the old engine and passes on the fix (where possible).
3. Compare A/G vocals, both modes, 44.1/48/96 kHz, 1/127/512-sample blocks, near-dual-mono and true stereo; check automation/reset transitions and reported delay.
4. Provide median/p90/p95/p99 cents, nearest-note vs commercial-consensus error, quiet windows, float peak, mono-fold loss, waveform-step proxy, and CPU cost, with repeatable commands and caveats.
5. Do an actual Windows JUCE VST3/Standalone build and FL Studio round-trip/PDC test if a Windows host is available; otherwise explicitly leave it unverified.
6. Summarize accepted/rejected experiments, limitations, and next steps in the change description.
