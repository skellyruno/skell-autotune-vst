#include "PitchShifter.h"
#ifdef NDEBUG
#error "The DSP regression suite must execute assertions even in Release builds"
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

// Real-time causal regression: identical control events and input samples must
// yield identical results regardless of the DAW callback partition. Abrupt
// notes, vibrato and aperiodic consonant energy exercise grain matching.
static std::vector<float> render(double sr, int blockSize, bool lowLatency)
{
    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(100.0);
    shifter.setLowLatencyExperiment(lowLatency);
    if (lowLatency) shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(false);
    shifter.setPhaseLockedSource(true);

    const int total = static_cast<int>(sr * 1.6);
    const int noteChange = static_cast<int>(sr * 0.58);
    const int noiseStart = static_cast<int>(sr * 1.03);
    const int noiseEnd = static_cast<int>(sr * 1.10);
    std::vector<float> samples(static_cast<size_t>(total), 0.0f);
    double phase = 0.0;
    unsigned int randomState = 71237;
    for (int i = 0; i < total; ++i)
    {
        const double f = (i < noteChange ? 205.0 : 297.0)
                       * (1.0 + 0.005 * std::sin(i * 2.0 * 3.141592653589793 * 5.0 / sr));
        phase += 2.0 * 3.141592653589793 * f / sr;
        const double harmonic = 0.22 * std::sin(phase) + 0.11 * std::sin(2.0 * phase + 0.11)
                               + 0.045 * std::sin(3.0 * phase + 0.17);
        randomState = randomState * 1664525u + 1013904223u;
        const float noise = static_cast<float>((randomState >> 8) / 16777216.0 - 0.5);
        samples[static_cast<size_t>(i)] = static_cast<float>(harmonic)
            + ((i >= noiseStart && i < noiseEnd) ? 0.12f * noise : 0.0f);
    }

    int cursor = 0;
    while (cursor < total)
    {
        // Controls update at the *same sample index* for both block sizes.
        int n = std::min(blockSize, total - cursor);
        for (int edge : {noteChange, noiseStart, noiseEnd})
            if (cursor < edge) n = std::min(n, edge - cursor);
        const double f = cursor < noteChange ? 205.0 : 297.0;
        const float ratio = cursor < noteChange ? 1.0594631f : 0.9438743f;
        shifter.process(samples.data() + cursor, n, static_cast<float>(sr / f), ratio);
        cursor += n;
    }
    return samples;
}

int main()
{
    for (double sr : {44100.0, 48000.0, 96000.0})
    {
        for (bool lowLatency : {false, true})
        {
            const auto a = render(sr, 127, lowLatency);
            const auto b = render(sr, 512, lowLatency);
            assert(a.size() == b.size());
            double worst = 0, rms = 0, maxPeak = 0;
            for (size_t i=0; i<a.size(); ++i)
            {
                assert(std::isfinite(a[i]) && std::isfinite(b[i]));
                worst = std::max(worst, std::abs(static_cast<double>(a[i])-b[i]));
                maxPeak = std::max(maxPeak, std::abs(static_cast<double>(a[i])));
                if (i > static_cast<size_t>(sr * 0.9) && i < static_cast<size_t>(sr * 1.0))
                    rms += static_cast<double>(a[i]) * a[i];
            }
            // The phase matcher may work on different recorded cycles, but it
            // must not change results just because the host buffer changed.
            assert(worst < 1.0e-5);
            assert(rms / sr > 1.0e-4);
            assert(maxPeak < 1.5);
            std::printf("%.0f Hz / mode=%d: block diff %.9g, peak %.4f\n", sr, lowLatency, worst, maxPeak);
        }
    }
}
