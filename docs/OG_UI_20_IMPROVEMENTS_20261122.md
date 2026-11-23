# OG SkellyTune UI — 20+ improvements, original VST3 candidate

Approved design constraint: **do not resize the pitch graph** (still
380–444 design Y, note traces 410–442). Leave original retune direction and
display: clockwise=faster, 0=instant/hard tune, 100=slow/natural. Keep host
automation IDs, scale presets, note toggles, and plugin identity intact.

## Requested
- Shorten black and white keyboard with retained 12-note toggles.
- Refine always-visible skull/skeleton with softer motion, responsive
  palette lighting, and compact target note rather than replacing bones.
- Enlarge numeric Retune/Humanize values; improve small UI controls.

## Additional independently coded refinements
1. Center white piano keys within compact frame
2. Center black keys on same baseline
3. Keep detected pitch marker aligned to key height
4. Sharpen ALLOWED NOTES heading contrast
5. Make brand wordmark accent material-aware
6. Normalize backdrop gradient for proportional window scaling
7. Give the Retune readout an accessible name
8. Give Humanize readout an accessible name
9. Apply high-contrast numeric readout colors in light/dark modes
10. Enhance targeted piano key hue
11. Enhance targeted piano key fill
12. Increase keyboard focus outline prominence
13. Increase blocked black key state visibility
14. Increase blocked white key state visibility
15. Enlarge allowed-note bars
16. Add thickness to key status bars
17. Enlarge natural/accidental key note labels
18. Adjust label-to-indicator spacing
19. Dim disabled combo arrows
20. Enlarge popup menu hit targets
21. Enlarge combo text
22. Improve Settings gear hover
23. Use theme-aware dynamic orb lights
24. Reduce bone scatter so vocal activity keeps recognisable bones
25. Reduce excessive skeleton sway at high activity
26. Add low-cost audio-responsive inner halo
27. Improve skull brow/teeth details
28. Replace giant note glyph with small target-note badge
29. Expand A4 decrement and increment hit areas
30. Expand theme, settings and power button hit areas
31. Provide accessible names for all 12 piano keys
32. Verify retune readout endpoints on the real attached JUCE slider
33. Verify native compact piano/readout geometry at all four window sizes

The source is **implemented**. DSP/audio processing is unchanged. Exact
head Windows VST3, pluginval, 52 CTests and visually inspected real JUCE
screenshots are required before declaring verified. No published or merged
release is authorized.

## Follow-up native rendering checks

- Standard selected popup items now draw actual checkmarks (previously only tinted text).
- The Windows smoke test also captures a real JUCE editor snapshot with **injected E-note/high-activity orb state**, to inspect the active-note badge and persistent skeleton. This is a visual test with synthetic UI state, not an actual recorded vocal render.

## Real screenshot inspection — corrective patch

The first active-note screenshot **failed visual inspection**: despite a
relative scatter cap, orbit distance still moved limbs tens of pixels,
creating a fragmented skeleton. The root fix keeps each bone stroke whole
and caps actual Euclidean translation at **2.5 design pixels** for major
bones or **1.5** for details. Audio reactivity remains, but the character
should keep its continuous silhouette. The same pass slightly increases
header label typography for 420px compact-window readability. Both are
UI-only, no audio-thread/DSP changes. Recheck latest real active-note
screenshot and run exact-head Windows VST3/pluginval/52 DSP tests.

## Follow-up: rib-cage silhouette improvement

Actual 700px and 1050px JUCE screenshots after `7c9aa65` showed the
chest as a stack of angular zigzags although the skull and limbs remained
continuous. Replaced the old **70 individually drawn small rib strokes**
with **10 smooth cubic curves** (five complete ribs on each side) and
subpixel, activity-linked breathing. The rest of the skeleton geometry,
all DSP/host controls, note badge and existing screenshot scenes remain
unchanged. **Visual result and UI CPU remain unverified until the new
Windows-runner screenshots are inspected.**
