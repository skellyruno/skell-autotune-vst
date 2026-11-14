#include "NoteTransition.h"
#include <cmath>
#include <cstdio>

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
    return ok ? 0 : 1;
}
