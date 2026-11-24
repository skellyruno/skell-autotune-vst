# OG SkellyTune — uppercase two-tone brand, smaller knobs, black chassis

Canonical `skellyruno/skell-autotune-vst` draft PR #10, continuing
from verified 3506d74 (not SkellyTune Mini).

## Requested and implemented in source
- **SKELLYTUNE** fully uppercase, no space, as one glyph run; hard
  chromatic separation at SKELLY/TUNE using actual measured glyph widths.
  White SKELLY, green TUNE, including each existing theme's green variant.
- Both existing Retune and Humanize rotary controls shrink from
  152x152 to 137x137 design coordinates (~9.87% smaller). Their centers,
  readouts, clockwise direction, 0=instant/100=slow text, APVTS ids
  and saved automation are preserved.
- The editor exterior is **exact solid black (#000000)** in every theme.
  Material panels use subtly different near-black colors and inherit
  meaningful accent/metal variations. Labels that were previously dark
  on light surfaces become bright for contrast.
- Move fine satin/brushed marks inside clipped panels; avoid tinting
  the black chassis. Retain controlled orb lightning and head/scatter.

## Mandatory verification
Windows JUCE screenshot smoke now checks both real rotary sizes across
all four viewport sizes, solid black exterior pixels in all four
themes, actual different panel material pixels, original retune mapping,
stacked A4 control geometry, and native lightning/active-note scenes.
The prior test expecting differently colored exterior backgrounds is
updated deliberately because this new user-approved black style
supersedes it.

Full Windows VST3, pluginval strictness 5, offline 53 DSP regression tests,
visual inspection and screenshot correction are still required on
this exact head. Real FL Studio and actual G/A vocal A/B remain
unverified; do not alter production parameters or distribute publicly.
