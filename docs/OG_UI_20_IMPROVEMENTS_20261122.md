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
