#include "PitchShifter.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifdef NDEBUG
#error "Frequency slew regression requires active assertions"
#endif

namespace humtune {
struct PitchShifterSpectralOracle {
    static void check(double rate, int block) {
        PitchShifter shifter;
        shifter.prepare(rate, block);
        shifter.setMinFrequency(80);
        for (bool low : {false, true, false, true}) {
            shifter.setLowLatencyExperiment(low);
            // Re-prepare while the live mode is already selected too: cached
            // coefficients must follow both sample rate and persistent mode.
            shifter.prepare(rate, block);
            const int delay = shifter.getLatencySamples();
            shifter.livePhaseInitialized = true;
            shifter.livePhase = .25;
            shifter.liveFrequency = 200;
            shifter.processLiveSpectrumSample(0.0f, rate / 200.0, 1.5);
            const double firstStep = shifter.liveFrequency - 200.0;
            assert(firstStep > 0);
            if (low) {
                const double original = 300.0 - 100.0 * std::exp(-1.0 / (rate * .00015));
                assert(std::abs(shifter.liveFrequency - original) < 1e-10);
            } else {
                // Bound phase curvature at a 200->300 Hz target jump. The old
                // 0.15-ms Standard clock exceeds 600,000 Hz/s; the new clock
                // stays below 400,000 Hz/s at all supported test rates.
                if (firstStep * rate >= 400000.0) {
                    std::fprintf(stderr, "Standard target-clock curvature too high: %.3f Hz/s\n", firstStep * rate);
                    std::exit(1);
                }
            }
            assert(std::abs(shifter.livePhase - (.25 + shifter.liveFrequency / rate)) < 1e-12);
            assert(shifter.getLatencySamples() == delay);
        }
    }
};
}

int main() {
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (int block : {1, 127, 512})
            humtune::PitchShifterSpectralOracle::check(rate, block);
    std::puts("Frequency slew: bounded Standard curvature, unchanged live response, phase continuity and delay passed");
}
