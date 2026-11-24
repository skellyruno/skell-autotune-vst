# OG SkellyTune — manual formant direction and hardware refinement

Continue draft PR #10 from verified 955e22f, not Mini. Sound contract:
left/down negative shifts vocal-tract resonance lower (deeper/darker),
right/up positive shifts resonance higher (brighter/higher-sounding),
zero neutral. This is not a note transposer. Existing DSP mapping is
already exp2(semitones/12); changing its sign would invert the intended
operation. Preserve the published -4..+4 st parameter ID/range/default
and the separate Formant Preserve setting. Do not claim identical sound
to any third-party commercial plugin.

New full-engine synthetic voiced-vowel regression checks resonance
centroid ordering at -4/0/+4 st, already-tuned 220 Hz input, fundamental
periodicity and detector/target frequencies, finite peaks, unchanged
reported latency. It exercises both latency and preserve modes across
44.1/48/96 kHz with 127/1/512-sample blocks. Verbose CI reports the actual
measurements. No DSP coefficients were changed without evidence of a
failure. If a configuration fails, diagnose the actual rendered audio.

UI: explicit signed DEEPER/NEUTRAL/BRIGHTER readout and engraved endpoints,
clear tooltip, finer physical dial machining/raised lip, cached satin
texture, more restrained curved glass reflection and branched lightning.
Native smoke checks real APVTS signs, center position, labels and both
extreme screenshots. Preserve the existing trace pending choice, black
chassis, character morph, knob sizes, piano, Retune and project identity.

Prior 955e22f Windows/DSP/editor/pluginval jobs passed. Its 12 actual
captures were opened; the Humanize morph now joins limbs. New exact-head
build, real screenshot inspection, and real-vocal/FL Studio remain
separate gates. No claim of perceptual parity or owner release approval.
