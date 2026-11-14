# SkellyTune 1.1.2 Beta 5

SkellyTune is a JUCE vocal pitch corrector with Standard and experimental Low Latency modes, formant preservation, and a compact editor. This version includes the 700 × 680 layout with A4 in the pitch readout, revised piano, smoother Standard pitch transitions, and an SSE2 pitch-detector optimization.

Build the Windows VST3 and Standalone with Visual Studio and CMake:

```sh
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo --parallel
```

Run portable DSP checks:

```sh
cmake -S . -B build-dsp -DSKELLYTUNE_DSP_TESTS_ONLY=ON
cmake --build build-dsp --config Release --parallel
ctest --test-dir build-dsp -C Release --output-on-failure
```

The full 60-test suite uses nine real-vocal regressions whose audio excerpts are supplied separately. CMake reports which tests require those local fixtures. The beta has known transition outliers on the G vocal and callback spikes during fast automation at 96 kHz / 64 samples.
## Automatic SkellKey Send to Tune (Windows)

Insert SkellKey and SkellyTune in your DAW, let SkellKey detect a key and
click **SEND TO TUNE**. SkellyTune automatically receives Key, Major/Minor
Scale and A4 through a built-in same-session SkellCore shared-memory link.
**No Hub, Python, third-party app, internet or extra plugin control is needed.**
Unconfirmed keys are not sent. The message-thread receiver updates the
existing `rootNote`, `scaleType`, `scaleExtension` and `referenceFreq`
automation parameters and keeps an explicitly customized 12-note piano mask
untouched. Presets, plugin identity and audio-thread DSP are unchanged.
Normal sender shutdown withdraws its announcement; crashes expire after five
seconds. The optional legacy Hub remains available on non-Windows hosts.
