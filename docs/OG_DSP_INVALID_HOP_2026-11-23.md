# OG SkellyTune — targeted invalid detector-hop guard

**Reproduced original defect:** `NoteTransition::choose` reset motion
history on invalid observed pitch, predicted pitch, confidence or hop,
but returned `measuredTarget` even when a different note had already
been accepted. A valid committed target 62 plus one invalid observation
with measured target 64 yielded 64 immediately: unsupported target jump.

**Repair:** return the last accepted note for invalid pitch/time frames;
clear stale velocity evidence and guard nonfinite durations; if no prior
target exists, use a finite measured target or safe MIDI 69 fallback.
The next valid high-confidence measurement still updates immediately.
No changes to the synthesis algorithm, normal reliable-pitch decisions,
Retune mapping, parameter IDs, PDC, presets or saved sessions.

The added regression must fail on the original invalid-return path and
pass on this fix. Assess with exact-head offline CTests, Windows JUCE VST3,
pluginval, and real G/A vocal listening before claiming audible quality.
