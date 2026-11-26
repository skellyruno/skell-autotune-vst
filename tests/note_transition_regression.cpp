#include "NoteTransition.h"
#include <cmath>
#include <cstdio>
#include <limits>

int main()
{
    using humtune::NoteTransition;
    bool ok = true;
    auto expect = [&](bool pass, const char* name)
    {
        std::printf("%s: %s\n", name, pass ? "PASS" : "FAIL");
        ok = pass && ok;
    };
    NoteTransition t;
    constexpr double hop = 0.004;
    expect(t.choose(62.55f, 62.55f, 62.f, 62.f, .99f, hop, false) == 62.f,
           "stable note starts at measured scale target");
    // Two confident monotonic hops toward the boundary between D and E.
    t.choose(62.70f, 62.90f, 62.f, 62.f, .97f, hop, false);
    const float anticipated = t.choose(62.90f, 63.19f, 62.f, 64.f, .97f, hop, false);
    expect(anticipated == 64.f, "confident approaching note can change at boundary");
    // Trust a clean onset at a boundary one hop earlier, but never let
    // unrelated low-confidence spikes use the onset fast path.
    t.reset();
    t.choose(62.70f,62.85f,62.f,62.f,.99f,hop,false,true);
    const float onsetEarly=t.choose(62.90f,63.21f,62.f,64.f,.99f,hop,false,true);
    expect(onsetEarly==64.f,"trusted transient crosses one hop early");
    t.reset();
    t.choose(62.70f,62.85f,62.f,62.f,.99f,hop,false,true);
    const float onsetWeak=t.choose(62.90f,63.21f,62.f,64.f,.48f,hop,false,true);
    expect(onsetWeak==62.f,"weak-confidence onset cannot force note");
    // Stationary pitch in the same ambiguous pitch-class region shouldn't oscillate.
    t.reset();
    for (int i=0;i<20;++i)
    {
        const float v = 62.83f + (i%2 ? .006f : -.006f);
        const float target = t.choose(v, 63.22f, 62.f, 64.f, .98f, hop, false);
        if (target != 62.f) ok = false;
    }
    expect(ok, "static vibrato/jitter does not trigger anticipatory changes");
    t.reset();
    t.choose(62.f,62.f,62.f,62.f,.98f,hop,false);
    const float low = t.choose(74.f,74.f,74.f,74.f,.40f,hop,false);
    expect(low == 62.f, "single low-confidence octave error rejected");
    const float real = t.choose(69.f,69.f,69.f,69.f,.99f,hop,false);
    expect(real == 69.f, "high-confidence real leap does not require wait");
    t.reset();
    t.choose(62.70f,62.9f,62.f,62.f,.99f,hop,true);
    const float stabilized = t.choose(62.9f,63.2f,62.f,64.f,.99f,hop,true);
    expect(stabilized == 62.f, "optional Note Stabilizer remains authoritative");
    // A rejected early prediction must not poison the following confidence hold.
    t.reset();
    t.choose(62.55f,62.55f,62.f,62.f,.99f,hop,false);
    t.choose(62.70f,62.90f,62.f,62.f,.99f,hop,false);
    const float vetoed=t.choose(62.90f,63.20f,62.f,64.f,.99f,hop,false,false,false);
    expect(vetoed==62.f,"recent source turn vetoes forecast crossing");
    const float afterVeto=t.choose(62.91f,63.21f,62.f,64.f,.50f,hop,false);
    expect(afterVeto==62.f,"confidence dropout holds accepted target after veto");

    // Reproduced failure: two untrusted detector hops moving toward a
    // boundary made the NEXT trusted hop look like confirmed acceleration.
    // The old motion history wrongly forecast D -> E (62 -> 64) immediately.
    t.reset();
    t.choose(62.55f,62.55f,62.f,62.f,.99f,hop,false);
    t.choose(62.68f,62.93f,62.f,62.f,.40f,hop,false);
    t.choose(62.79f,63.08f,62.f,62.f,.40f,hop,false);
    const float afterWeakRamp=t.choose(62.90f,63.21f,62.f,64.f,.99f,hop,false);
    expect(afterWeakRamp==62.f,
           "untrusted pitch ramp cannot invent confirmed note-crossing motion");
    expect(t.choose(63.00f,63.25f,62.f,64.f,.99f,hop,false)==62.f,
           "first trusted follow-up still needs motion confirmation");
    expect(t.choose(63.10f,63.31f,62.f,64.f,.99f,hop,false)==64.f,
           "three consecutive trusted moving hops still forecast promptly");
    t.reset();
    t.setReferenceStride(2);
    t.choose(62.55f,62.55f,62.f,62.f,.99f,.002,false);
    t.choose(62.65f,62.65f,62.f,62.f,.99f,.002,false);
    t.choose(62.75f,62.95f,62.f,62.f,.99f,.002,false);
    t.choose(62.82f,63.11f,62.f,62.f,.99f,.002,false);
    expect(t.choose(62.91f,63.23f,62.f,64.f,.99f,.002,false)==64.f,
           "2ms detector hop preserves genuine 4ms motion evidence");
    // A 2 ms -> 4 ms detector-cadence switch used to interpret earlier 2 ms
    // deltas as fresh 4 ms motion, and could predict the upper note one hop
    // before any observation at the new cadence.
    t.reset();
    t.setReferenceStride(2);
    for (const float observed : {62.50f, 62.62f, 62.74f, 62.86f, 62.94f})
        t.choose(observed, observed, 62.f, 62.f, .99f, .002, false);
    t.setReferenceStride(1);
    const float afterCadenceChange = t.choose(63.00f, 63.24f, 62.f, 64.f, .99f, .004, false);
    expect(afterCadenceChange == 62.f,
           "2ms to 4ms cadence change discards stale velocity evidence");
    t.choose(63.04f, 63.26f, 62.f, 64.f, .99f, .004, false);
    expect(t.choose(63.08f, 63.29f, 62.f, 64.f, .99f, .004, false) == 64.f,
           "verified 4ms trajectory can forecast again after cadence change");

    // Invalid detector values also break motion continuity. The old path
    // returned a fallback note but left pre-gap velocity fully trusted.
    t.reset();
    t.setReferenceStride(1);
    t.choose(62.55f, 62.55f, 62.f, 62.f, .99f, .004, false);
    t.choose(62.75f, 62.90f, 62.f, 62.f, .99f, .004, false);
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    expect(t.choose(invalid, invalid, 62.f, 62.f, .99f, .004, false) == 62.f,
           "invalid detector estimate returns measured target");
    expect(t.choose(62.93f, 63.21f, 62.f, 64.f, .99f, .004, false) == 62.f,
           "first reliable estimate after invalid gap does not forecast early");
    // Regression: invalid confidence/pitch/time must not replace a
    // previously accepted target. Old source returned measuredTarget=64
    // immediately even though it only had evidence for accepted note 62.
    t.reset();
    t.choose(62.55f, 62.55f, 62.f, 62.f, .99f, .004, false);
    expect(t.choose(invalid, 65.f, 64.f, 64.f, .99f, .004, false) == 62.f,
           "invalid observed pitch cannot force unverified target jump");
    expect(t.choose(62.9f, invalid, 64.f, 64.f, .99f, .004, false) == 62.f,
           "invalid forecast cannot change last accepted target");
    expect(t.choose(62.9f, 63.2f, 64.f, 64.f, invalid, .004, false) == 62.f,
           "invalid confidence cannot change last accepted target");
    expect(t.choose(62.9f, 63.2f, 64.f, 64.f, .99f,
                    std::numeric_limits<double>::quiet_NaN(), false) == 62.f,
           "invalid elapsed hop cannot change last accepted target");
    expect(t.choose(64.2f, 64.2f, 64.f, 64.f, .99f, .004, false) == 64.f,
           "next confident note responds immediately");
    t.reset();
    expect(std::isfinite(t.choose(invalid, invalid, invalid, invalid, invalid,
                                  std::numeric_limits<double>::quiet_NaN(), false)),
           "invalid startup observations do not emit NaN");
    return ok ? 0 : 1;
}
