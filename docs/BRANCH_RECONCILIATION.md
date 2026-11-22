# OG SkellyTune — branch reconciliation (2026-11-22)

This repository is **OG SkellyTune**, not SkellyTune Mini. Canonical active
draft: [PR #10](https://github.com/skellyruno/skell-autotune-vst/pull/10),
branch `fix/skellytune-og-trusted-motion-20261122`, targeting the current
default `devin/1778052783-initial-plugin`.

No PR has been merged and no branch has been deleted. The GitHub integration
used for the audit cannot delete refs. Archive first; deletion is a separate
reviewed operation. Do not force-push or remove a branch with unique commits.

| Branch | Audited tip | Relative to PR #10 before this document | Recommendation |
|---|---|---|---|
| `devin/1778052783-initial-plugin` | `b1421fe29faf` | production default ancestor | **Retain default**, never auto-delete |
| `fix/skellytune-og-trusted-motion-20261122` | `e88881634095` | current reviewed candidate | **Retain active draft**, refresh SHA after next batch |
| `codex/skellytune-current-beta` | `719117636cfb` | contained by ancestry (25 behind) | Eligible for retirement **only after** preserving external users/CI pointers |
| `codex/compact-live-editor` | `7f9d88040277` | contained by ancestry (16 behind) | Same as above; closed PR #4 |
| `fix/skellytune-release-ci-guard-20261119` | `d72c2de3d276` | contained by ancestry (7 behind) | Same as above; closed PR #6 |
| `fix/skellytune-dsp-regressions-combined-20261120` | `f3e58007777c` | contained by ancestry (1 behind) | Same as above; direct tested parent |
| `release/skellytune-vst3-package-integrity-20261119` | `921082176ace` | divergent: 2 ahead, 10 behind | **Keep**: release/public-artifact behavior differs; closed PR #5 |
| `fix/skellytune-dsp-regressions-20261120` | `dc9474021037` | divergent: 1 ahead, 10 behind | **Keep**: compare DSP source against integrated line; closed PR #7 |
| `fix/public-vst3-artifacts-20261120` | `6714f13eeb0c` | divergent: 3 ahead, 10 behind | **Keep**: inspect artifact privacy and release gating |
| `feat/hubless-skellkey-tuning-20261111` | `369c2580f3bf` | divergent: 6 ahead, 28 behind | **Keep**: compare cross-plugin SkellKey message protocol/compatibility |
| `fix/canonical-skellcore-reference` | `79159e80663b` | divergent: 1 ahead, 27 behind | **Keep**: inspect pinned shared-component dependency |
| `codex/skellytune-low-latency` | `8082754b79ef` | divergent: 4 ahead, 36 behind | **Keep**: old DSP/UI and real screenshot evidence, avoid regressions |

The 'ahead/behind' counts describe **commit ancestry**, not number of useful
unique features. A divergent tip may share the same intended fix through a
different commit or may contain changes that should **not** be adopted. Use
per-file review and full DSP/Windows checks before porting anything.

## Audited decision

- Closed superseded PRs #4–#7 and retained their review histories.
- Kept the single active draft PR #10. Do not merge on passing CI alone.
- Do **not** copy the old public Windows VST3 artifact upload back into the
  active workflow; the public-repository product ZIP remains deliberately
  withheld until owner authorization.
- Prevent new feature PR proliferation: add related checked fixes to #10 in
  coherent batches; keep separate evidence branches only for incompatible
  experiments.
- For a future branch prune, verify its tip is an ancestor of an approved,
  durable target; verify no protected/release/manual CI is in flight; preserve
  the exact tip SHA and check any linked PR or external consumer. Prune
  ancestors only. Never delete the six divergent refs without reconciling
  their unique changes, and never delete default/release refs automatically.
