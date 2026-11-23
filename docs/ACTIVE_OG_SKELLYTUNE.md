# OG SkellyTune — active audit and release gates

**Repository:** `skellyruno/skell-autotune-vst` (not Mini)
**Canonical draft:** [PR #10](https://github.com/skellyruno/skell-autotune-vst/pull/10)
**Audited exact-head baseline:** `ef0a3c842221ccea684a4dda87ee01ee794b2433`
**Baseline CI:** [run 45083672635](https://github.com/skellyruno/skell-autotune-vst/actions/runs/45083672635)

## Verified at audited baseline

- Windows x64 JUCE VST3 compiled, pluginval strictness 5 passed.
- Offline Release 52/52 CTests passed, including 2ms/4ms note-transition regression.
- Packaging produced strict x64 engineering preview on the Windows runner.
- Original RETUNE mapping was restored **byte-for-byte** in approved
  `Source/PluginEditor.cpp`/`.h`: clockwise faster,
  on-screen **0 instant / 100 slow**. APVTS `retune=1` is fastest,
  `AutoTuneEngine::setRetuneSpeed(1-retune)` receives zero at hard tune.
  **Do not reverse this knob again** without tracing the whole path and
  reproducing a real failure. Stored IDs and host program state unchanged.
- Four real JUCE screenshot files were captured, but the earlier smoke
  harness **only changed the processor's theme value while the editor
  was hidden**. `timerCallback()` exits if not showing. The old
  screenshots therefore depicted the same green/dark theme in each file:
  the files prove layout, not four distinct themed appearances. A new
  smoke harness exercises the actual `Cycle theme` button callback
  and checks sampled rendered background colours are distinct; treat
  that additional proof as **unverified until exact-head CI passes**.
- The correct candidate CI withholds public installable VST3 artifacts.
  An older separately verified November 20 VST3 artifact is not the
  current candidate.

## Not yet validated for release

- FL Studio load, remove, close/reopen, project/preset/state and automation
  roundtrip, exact live PDC per range/mode, A4 and SkellKey behavior.
- Real G/A dry-vocal A/B vs reference renders, tonal/formant integrity,
  median/p95/p99 cents under frozen masks, consonant starts and callback CPU.
- Host-affine UI interactions: theme button, settings menu, resize/DPI,
  powered/bypass UI, animated orbit, note lights while audio plays.
- Commercial rights, vendor/license/EULA and user-approved release identity.

## UI inspection and potential improvements (NOT IMPLEMENTED)

From the real 420x408 / 476x462 / 700x680 / 1050x1020 captures,
prioritize these **without changing the approved retune control, parameter
mapping, or saved sessions**:

1. **Pitch trace becomes useful.** The live original/target history is
   drawn into only ~31 design-coordinate px (less at 420w).
   Increase the graph's height, provide note/cents grid reference,
   stronger trace contrast and optional expanded pitch view.
   Reduce the oversized lower keyboard area rather than shrinking knobs.
2. **Readable numeric status.** Retune/Humanize values are tiny under
   large rotary metal faces, especially at 420x408. Enlarge numeric
   readouts and add one concise mode/bypass indication, not extra prose.
3. **Hero skeleton.** Retain the already approved skull/skeleton motif.
   Increase crispness and material depth, use subtle audio-reactive
   illumination informed by actual input/voicing, and tone down the
   broad green surface glow. Avoid performance-heavy background repaints.
4. **Keyboard information density.** The large keyboard dominates the
   bottom quarter despite being a 12-note mask. Reduce its height,
   strengthen allowed/target/original note states, retain manual
   toggling and saved note IDs.
5. **Small-window usability.** Improve A4 increment hit targets,
   gear/power hit areas, combo box label legibility, hover/focus states,
   tooltip wrapping and 125/150/200% DPI. Do not hide existing controls.
6. **Theme fidelity QA.** Verify each material palette (especially
   light theme 3) actually renders distinctly after a user click, with
   genuine before/after captures, not mislabeled fixed-theme screenshots.

Keep separate screenshot → inspect → fix → recapture evidence and real
FL Studio verification. This document audits, **not approves**, a redesign.

## Branch consolidation and review policy

See [BRANCH_RECONCILIATION.md](BRANCH_RECONCILIATION.md) for the exact
12-branch preservation inventory. Four historical branches are
ancestry-contained and candidates for later cleanup; six divergent
branches must not be deleted or silently merged. **One active draft PR,
no merge.** The public artifact gate must not be weakened just to
distribute a preview; private download requires a safe delivery route.
Historical Oct v39 reports/28-test instructions are preserved rather
than rewritten as if they described the current 52-test candidate.
