# OG SkellyTune UI — November 23, 2026

One-word SkellyTune branding, vertically stacked smaller black A4 +/-
on the right, refractive 3D orb lighting and audio-reactive electrical
discharges behind the scattered skeleton, analog machined knob face,
calibration ticks and subtle satin body/chassis texture.

No pitch-graph resizing; no modifications to knobs' parameter direction,
DSP, VST3 identity, preset automation IDs or saved state. Editor smoke
runs on actual Windows JUCE VST3 code and checks A4 placement, real
themes, retune 0=instant/100=slow and a lightning-active screenshot.
Real FL Studio, visual inspection and Windows CI remain separate gates.

## Screenshot inspection — badge clipping fix

The first combined exact-head lightning-active screenshot was inspected.
The E-note indicator at (190,200,45,43) was partially cut by the orb's
122px circular glass mask. The badge has moved to (194,163,42,40):
its furthest lower-right corner remains inside the clip, preserving
the user's active scattered skeleton and lightning. Rebuild and inspect
the new actual JUCE screenshot before declaring this visual repair done.
