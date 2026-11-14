#include "NeutralBypassGate.h"
#include <cmath>
#include <cstdio>

int main()
{
    humtune::NeutralBypassGate gate;
    auto ratio = [](double cents) { return static_cast<float>(std::exp2(cents / 1200.0)); };
    // Baseline's <=1.4-cent snap changed 180+ times on one recorded vocal.
    // The gate must ignore alternating near-zero and just-over-zero samples.
    int edges = 0;
    bool previous = false;
    for (int i=0; i<150; ++i)
    {
        bool now = gate.update(ratio(i%2==0 ? 0.6 : 2.0), .004, false);
        edges += now != previous; previous = now;
    }
    if(edges != 0 || gate.isLatched())return 1;
    if(gate.update(ratio(.5), .004, false))return 2;
    if(!gate.update(ratio(.5), .004, false))return 3;
    // Once stable, a 2-cent detector wobble must not start another crossfade.
    for (int i=0;i<100;++i)
        if (!gate.update(ratio(2.0), .004, false))return 4;
    if (gate.update(ratio(4.0), .004, false))return 5;
    // A vocal with independent formant processing cannot take unity dry bypass.
    gate.update(ratio(0.0),.012,false);
    if(gate.update(ratio(0.0),.004,true))return 6;
    // Invalid controls are rejected, rather than latching forever.
    if(gate.update(NAN,.004,false))return 7;
    gate.update(ratio(0.0),.008,false);
    gate.reset();
    if(gate.isLatched())return 8;
    std::puts("neutral dry bypass hysteresis: PASS");
    return 0;
}
