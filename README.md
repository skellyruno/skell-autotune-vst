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
| **Low Latency Mode (Experimental)** | Live overlap-normalized resynthesis with a nominal 1.9 ms shifter budget, transparent unchanged/unvoiced audio, and smoothed cycle joins |
| **Pitch Orb Display** | Real-time visual feedback showing detected note and correction direction |
| **In/Out Heatmap** | Scrolling pitch history showing detected vs corrected pitch |
| **12 Presets** | Hard Snap, Sharp Correct, Natural, R&B Smooth, Trap Vocal, Gospel, and more |
| **Studio Interface** | Resizable charcoal interface, mint accents, live input/target pitch monitor, correction ruler, and interactive scale keyboard |



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

Low Latency remains opt-in. Its shifter budget is 84 samples at 44.1 kHz (1.905 ms),
92 at 48 kHz, and 183 at 96 kHz. This is the plugin's reported audio delay, not
pitch-detection response time or complete device/DAW round-trip latency. The detector
still needs several cycles to identify a new note, especially in the bass ranges.

The low-latency shifter now starts live grains at their left edges and normalizes
simultaneous overlap. The former implementation could insert grains after their
left edges had passed, losing window weight and producing level modulation. Joins
between predicted cycles are crossfaded. At unity correction with neutral formant
shift, and for unvoiced audio, the signal follows the exact latency-matched dry path;
transitions into and out of correction are ramped over 3 ms. Standard mode's DSP is
unchanged.

Run the focused regressions without downloading JUCE:

```sh
cmake -S . -B build/low-latency -DCMAKE_BUILD_TYPE=Release -DSKELLYTUNE_LOW_LATENCY_TESTS_ONLY=ON
cmake --build build/low-latency --parallel 3
ctest --test-dir build/low-latency --output-on-failure
```

These checks cover exact delayed passthrough and startup transients, 48 harmonic
pitch-shift cases at 40/80/220/880 Hz across 44.1/48/96 kHz, integrated 223-to-220 Hz
correction at 44.1 kHz/96 samples, changing notes with vibrato, stereo phase,
32–512-sample callback consistency, callback allocation detection, mode resets and
sample-rate changes. The runner reports callback timing rather than enforcing a
machine-dependent wall-clock threshold. The older `SKELLYTUNE_DSP_TESTS_ONLY` option
references legacy test files absent from this revision; it is separate from the
focused suite above.

These are synthetic checks. Real-vocal artifacts, formant quality, Windows/macOS
host performance and device loopback latency still need listening and host validation.
For FL Studio at 44.1 kHz/96 samples, the audio buffer is about 2.18 ms per buffer;
interface and driver latency are additional. Avoid changing latency mode during a
take: the host must update delay compensation, and the DSP history resets on a mode
change.
