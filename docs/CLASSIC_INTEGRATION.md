# Standalone SkellyTune — Classic migration candidate

The active plugin processor now uses `ClassicEngine`, not the previous OG AutoTuneEngine. The VST3 identity, parameter IDs, preset indices and artwork remain unchanged. No replacement plugin or backup branch was created. The unchanged native SnapCore (SHA256 ca46212c500efe397968a2ef9869050f26b0ea015b5f462fd8ac99ffb6b3672b) is recovered from the exact Playground dependency. Classic mode 0 is fixed; Crystal is not modified. The standalone wrapper is not the Playground Bank wrapper and does not claim bit-identical output to that wrapper.

- Native retune: existing host 1 = fast maps to 0 ms, 0 = slow maps to 100 ms; native Humanize, pitch-class mask, reference, range and low-latency 2.5 ms / stable 10 ms are wired.
- Formant Shift uses the existing independent spectral colour stage when nonzero; it adds 1024 samples of latency. Neutral shift bypasses that stage. Dynamic PDC and transitions require host/listening validation.
- Classic has native formant behavior, not the old algorithm's separate preservation toggle; retain the former ID for saved sessions, remove the misleading UI toggle. Fractional scales currently project to semitones; glide and stabilizer map to smoothing, not the previous exact behavior. These are migration limitations, not release claims.
- Old OG source still exists in this standalone repository's offline regression harness until the active Classic tests replace its coverage. It is not linked as the active pitch engine. No new OG copy is created.
- Active tests compare the neutral path to the native core, check delay, bypass, telemetry and independent formant DSP. Windows build and FL Studio tests remain mandatory. Keep PR draft until verified; no stable release.
