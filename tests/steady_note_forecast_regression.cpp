#include "SteadyNoteForecast.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <iostream>

using humtune::SteadyNoteForecast;
static bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a-b) < tolerance; }
int main()
{
    SteadyNoteForecast state;
    const float observed = 60.10f, predicted = 60.30f;
    // An unconfirmed new note receives full prediction immediately.
    assert(near(state.process(observed, predicted, 60.0f, 0.99f, .004, false, true), predicted));
    // A stable note gradually damps the extrapolation after 24ms.
    float previous = predicted;
    for (int i=0; i<20; ++i)
    {
        const float next = state.process(observed, predicted, 60.0f, 0.99f, .004, false, false);
        assert(next <= previous + 0.00001f && next >= observed);
        previous = next;
    }
    assert(near(previous, observed + 0.46f*(predicted-observed)));
    // Onset, low confidence, or a new target always restore the full forecast.
    assert(near(state.process(observed,predicted,60.0f,.99f,.004,false,true),predicted));
    assert(near(state.process(observed,predicted,60.0f,.50f,.004,false,false),predicted));
    assert(near(state.process(observed,predicted,62.0f,.99f,.004,false,false),predicted));
    // Low Latency is unchanged after any length of a held note.
    for (int i=0;i<80;++i)
        assert(near(state.process(observed,predicted,62.0f,.99f,.004,true,false),predicted));
    state.reset();
    assert(near(state.process(observed,predicted,60.0f,.99f,.004,false,false),predicted));
    assert(std::isfinite(state.process(std::numeric_limits<float>::quiet_NaN(),predicted,60.0f,.99f,.004,false,false)));
    std::cout << "steady-note causal forecast passed\n";
}
