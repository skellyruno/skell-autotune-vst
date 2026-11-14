# Supplied onset/reentry sources

The 2026-10-24 adaptation uses the dropout-only onset-memory concept from the
user-supplied MIT SkellyTune Realtime v3.3 and the post-dropout periodicity
validation concept from Skelly MetaTune Base V1.11. The latter is supplied as
AGPL-3.0 source derived from MicFX/NoteForge. Its engine and CMNDF implementation
are not transplanted here: this integration uses SkellyTune's existing recorded
history correlation and its own bounded control state. These are engineering
provenance notes, not a legal determination. Original archives, licences and
attributions remain in work/incoming-2026-10-24; no commercial binary DSP is used.

New controls are enabled only for maximum hard-tune Low Latency with formants
off, following the user's current validation scope. Formants-on retains the
previous control path after the unrestricted prototype regressed A-major tails.
The existing parameter IDs, saved values and formant feature are preserved.
