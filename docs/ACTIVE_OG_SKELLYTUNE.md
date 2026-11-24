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

## Performance measurement follow-up (2026-11-22)

The current exact-head `f39a51e` passed [CI #45093669638](https://github.com/skellyruno/skell-autotune-vst/actions/runs/45093669638): 52/52 DSP cases, Windows VST3 target, 17/17 pluginval tests at strictness 5, strict packaging and native JUCE themed/active-state snapshots. The active-state screenshot was manually inspected: chest ribs have continuous curves and the skeleton remains intact. This does not establish FL Studio host performance or real vocal quality.

An informational CI step now executes the already-existing `realtime_stress_benchmark` (previously only compiled) and uploads a synthetic callback CSV. Matrix: 44.1 kHz/127-sample Standard; 48 kHz/128-sample Standard/Low Latency with formants off/on; 96 kHz/512-sample Low Latency. The signal is generated harmonics with fast ±pitch motion, 25ms formant automation, and a 0.5s warmup; `duty`, `p99_deadline_fraction`, float peaks and missed deadlines are measured on a shared Ubuntu runner. **Synthetic CI percentages are noisy and not direct measurements of user DAW CPU or real dry vocals.** Do not infer a speedup from those values without before/after runs on the same host. Exact-head new run pending.


## 2026-11-23 — owner-authorized Windows VST3 engineering download

Owner requested removal of the preview-binary artifact restriction for
installing and testing OG SkellyTune. The upload guard now additionally
allows this public repository's own PR **#10** from branch
`fix/skellytune-og-trusted-motion-20261122`. All other public PRs
and forks remain blocked; official releases continue to require legal,
commercial and identity approval, which is still pending. This is a
**testing-only** preview and not an approved sale/public release.

The Windows workflow still builds, runs native editor smoke, validates
pluginval strictness 5, and packages a checksum-verifiable AMD64 VST3
ZIP. The offline DSP regression must also pass before the downloaded
exact-head artifact is shared. Artifacts in this PUBLIC repository can
be accessible to others for up to 30 days. Do not claim FL Studio
roundtrip or real vocal listening from CI alone.
