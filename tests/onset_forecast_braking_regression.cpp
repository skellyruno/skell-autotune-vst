#include "NoteTransition.h"
#include <cstdio>

int main() {
    humtune::NoteTransition t;bool ok=true;constexpr double hop=.004;
    const auto expect=[&](bool pass,const char* message){std::printf("%s %s\n",pass?"PASS":"FAIL",message);ok&=pass;};
    // G18.60s-like historical pitch: rising but braking before the boundary.
    // The wide trusted-onset shortcut must not insert a one-hop upper target.
    t.choose(72.101f,72.6f,72,72,.99f,hop,false,true);
    t.choose(72.378f,72.9f,72,72,.99f,hop,false,true);
    t.choose(72.631f,73.10f,72,74,.99f,hop,false,true);
    expect(t.choose(72.835f,73.26f,72,74,.99f,hop,false,true)==72,
           "braking onset does not use expanded early-crossing shortcut");
    expect(t.choose(72.84f,73.27f,72,74,.50f,hop,false,true)==72,
           "brief confidence loss retains the accepted lower target");
    expect(t.choose(73.1f,73.3f,74,74,.99f,hop,false,true)==74,
           "measured real boundary crossing commits immediately");
    t.reset();
    t.choose(72.40f,72.7f,72,72,.99f,hop,false,true);
    t.choose(72.60f,72.9f,72,72,.99f,hop,false,true);
    expect(t.choose(72.82f,73.25f,72,74,.99f,hop,false,true)==74,
           "accelerating credible onset retains the early shortcut");
    t.reset();
    t.choose(72.6f,72.9f,72,72,.99f,hop,false);
    t.choose(72.8f,73.1f,72,74,.99f,hop,false);
    expect(t.choose(72.95f,73.2f,72,74,.99f,hop,false)==74,
           "ordinary confirmed near-boundary forecast remains available");
    t.reset();
    t.choose(72.6f,72.9f,72,72,.99f,hop,true,true);
    expect(t.choose(72.82f,73.25f,72,74,.99f,hop,true,true)==72,
           "Note Stabilizer remains authoritative");
    return ok?0:1;
}
