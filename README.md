# SkellyTune

Dedicated pitch correction (autotune) plugin built with
[JUCE](https://juce.com). Thick vocal sustain, sharp note snapping,
and formant-preserving TD-PSOLA pitch shifting.

Inspired by Slate Digital MetaTune, Antares Auto-Tune, and
Soundtoys Little AlterBoy.

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

