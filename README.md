# SkellyTune

Dedicated pitch correction (autotune) plugin built with
[JUCE](https://juce.com). Thick vocal sustain, sharp note snapping,
and formant-preserving TD-PSOLA pitch shifting.

Inspired by Slate Digital MetaTune, Antares Auto-Tune, and
Soundtoys Little AlterBoy.

Ships as:

- **VST3 plugin** — FL Studio, Ableton Live, Reaper, Cubase, Studio One, Bitwig
- **AU plugin** (macOS only) — Logic Pro, GarageBand
- **Standalone app** — runs without a DAW

## Features

| Feature | Description |
|---------|-------------|
| **YIN Pitch Detection** | High-accuracy monophonic pitch tracker with median filtering and confidence scoring |
| **TD-PSOLA Pitch Shifting** | Period-synchronous overlap-add for pitch correction |
| **Sub Bass Range** | Selectable 40 Hz floor (E1 range); existing range indices remain compatible |
| **Formant Preservation** | Keeps natural vocal character even with aggressive pitch shifts |
| **Scale Snap** | Major, Minor, Chromatic, or per-note custom scale with root key selection |
| **Retune Speed** | 0 ms (hard T-Pain snap) to 150 ms (gentle natural correction) |
| **Note Sustain** | Configurable cent threshold — once locked to a note, holds as long as vocal stays in range |
| **Note Stabilizer** | Ignores micro-fluctuations to prevent jittery correction |
| **Humanize** | Reduces correction amount for natural imperfection |
| **Low Latency Mode (Experimental)** | Opt-in fixed-delay resynthesis targeting a 1.9 ms shifter budget; not yet validated on real vocals or measured DAW round-trip latency |
| **Pitch Orb Display** | Real-time visual feedback showing detected note and correction direction |
| **In/Out Heatmap** | Scrolling pitch history showing detected vs corrected pitch |
| **12 Presets** | Hard Snap, Sharp Correct, Natural, R&B Smooth, Trap Vocal, Gospel, and more |
| **Purple UI** | HumHouse signature dark purple aesthetic inspired by MetaTune |



## Technical Design

### Why TD-PSOLA?

Previous implementations used fixed-size
grains (256 samples) for pitch shifting, which caused:
- Phase discontinuities at grain boundaries
- Metallic / robotic artifacts
- Formant shifting (chipmunk / barrel effect)

TD-PSOLA (Time-Domain Pitch-Synchronous Overlap-Add) solves all three:
1. **Grains are period-length** — aligned to the detected fundamental
2. **Hann-windowed overlap-add** — seamless transitions, no clicks
3. **Pitch changed via placement rate** — grain content (formants) stays intact

### Note Sustain Logic

Once the engine locks onto a scale note, it holds that note as long as
the incoming vocal stays within a configurable cent threshold (default
50 cents). This prevents jittery switching between adjacent notes on
sustained vocals and is key to getting that "painted" autotune sound.

## License

See [installer/eula.txt](installer/eula.txt).


## Low Latency Development Status

The experimental low-latency mode is off by default. Repository tests measure nominal ~1.9 ms scheduling and below-2 ms integrated sweep lag at 44.1/48/88.2/96 kHz for unity-ratio synthetic sweeps across several block sizes. The selectable Sub Bass range now reaches 40 Hz, including E1 (41.20 Hz), without changing the existing five saved voice-range indices. Synthetic harmonic/noisy E1→F#1 and changing-bass E1→G1 / E2→G2 cases pass across host block sizes, and integrated E1→F1 active correction passes at 44.1/48/88.2/96 kHz. The detector's YIN analysis now runs at approximately 12 kHz at every host rate, with a 31-tap low-pass FIR before decimation to reject aliasing. Regression tests include a strong 10.3 kHz interferer while tracking a 220 Hz fundamental at 44.1/48/96 kHz, plus 1,000 Hz and 1,450 Hz tracking. A detector-only GCC steady-clock benchmark measures about 9–19x lower elapsed time than the previous detector at the legacy 70 Hz floor across noisy repeated runs; the updated Sub Bass floor is more computationally expensive but remains far below the old full-rate search. These are synthetic DSP tests and detector-only timings, not full audio-callback, real-vocal or DAW certification. Repeated low-latency/standard mode changes are tested for stale-grain leakage; generation-stamped output slots avoid bulk ring clears during ordinary resets. Detector range changes invalidate history and wait for a fresh analysis window, and a 44.1-to-48 kHz re-prepare regression checks the sample-rate transition. Vocal artifact quality, formant behavior, DAW plug-in delay compensation, full callback CPU load and end-to-end device/DAW loopback still require validation. The full JUCE plugin build could not be run in the current environment because GitHub DNS resolution failed during dependency fetch. See [`LOW-LATENCY-CHECKLIST.md`](LOW-LATENCY-CHECKLIST.md) before using the experimental switch in performance.
