# Codex prioritized implementation backlog

## P0 — Establish trusted reproducibility before changing sound

- [ ] Run all 28 DSP regressions with assertions active.
- [ ] Inspect `AGENTS.md`, current APVTS defaults, all prior v39 report caveats.
- [ ] Extract separate `SkellyTune-Codex-Benchmark-Inputs.zip` at project root and rerun `python scripts/codex-benchmark.py --key both` (or record missing audio/dependencies).
- [ ] Freeze the benchmark mask, target note, sample rate, latency alignment, mode and voice-range settings. Record SHA-256 of all inputs.
- [ ] Build the actual Windows JUCE 8.0.4 VST3/Standalone; report failures honestly. Diagnose plugin load and UI open/close in FL Studio using real host logs if available.

## P1 — Improve the artifact root cause, not the headline number

- [ ] Trace every held-note/outlier event and separate detector error from note-selection error from grain-output drift.
- [ ] Test causal output oscillator/source-grain phase locking during target changes with transition-local continuity, no noncausal reads and no new unreported delay.
- [ ] Validate G and A both, both modes, stereo/near-dual-mono, 44.1/48/96 kHz, 1/127/512 blocks, on/off/range automation; explicitly compare p95 **and** p99.
- [ ] Add a regression that fails baseline and passes the focused fix. Keep all current tests green.

## P2 — Musicality and host readiness

- [ ] Fix remaining consonant/vowel artifacts and quiet voiced intervals without mixing uncorrected pitched audio or blanket gain.
- [ ] Trace >0 dBFS float peaks and repair their underlying cause; measure true peak and envelope changes before considering optional output limiting.
- [ ] Profile worst-case callback time (p99 under host block deadline), allocations and locks during parameter automation.
- [ ] Verify saved projects, presets, backwards-compatible parameter IDs, live display thread safety and exact VST3 PDC latency on a Windows host.
- [ ] Compare level-matched A/B renders blind against Auto-Tune Pro and all three XPitch modes; do not call a proxy score 'audible quality'.

## Required output from Codex

A code patch or PR; all test commands with pass/fail counts; a reproducible benchmark CSV showing improvements and regressions on **both vocals**; before/after lossless WAVs; concise changelog explaining what actually changed and why; a clear remaining-issues list. Do not declare completion based on unit tests alone.
