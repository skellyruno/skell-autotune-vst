#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Protect against the v28 failure mode: at a fixed 1.9 ms budget the old
// scheduler missed the left half of most grains. Normalizing the one remaining
// grain could not restore energy or phase. This test verifies actual coverage,
// contributors, and host block independence across sample rates and pitches.
struct Probe { std::vector<float> output; float coverage = 0.0f; float contributors = 0.0f; int latency = 0; };
static Probe render(double sr, double f0, float ratio, int blockSize)
{
    constexpr double pi = 3.14159265358979323846;
    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(70.0);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(false);
    Probe p;
    const int samples = static_cast<int>(sr * 0.8);
    p.output.resize(static_cast<size_t>(samples));
    p.latency = shifter.getLatencySamples();
    double sumCov = 0.0, sumCon = 0.0;
    int n = 0;
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / sr;
        p.output[static_cast<size_t>(i)] = static_cast<float>(0.18 * (
            std::sin(2*pi*f0*t) + 0.4*std::sin(4*pi*f0*t + 0.3)
                + 0.12*std::sin(6*pi*f0*t + 0.4)));
    }
    for (int i = 0; i < samples; i += blockSize)
    {
        const int m = std::min(blockSize, samples - i);
        shifter.process(p.output.data() + i, m, static_cast<float>(sr/f0), ratio);
        if (i > static_cast<int>(sr * 0.25) && i < static_cast<int>(sr * 0.7))
        {
            const auto g = shifter.getGrainDiagnostics();
            sumCov += g.coverage;
            sumCon += g.contributors;
            ++n;
        }
    }
    p.coverage = static_cast<float>(sumCov / std::max(1, n));
    p.contributors = static_cast<float>(sumCon / std::max(1, n));
    return p;
}
int main()
{
    bool success = true;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (double f0 : { 110.0, 220.0, 390.0 })
            for (float ratio : { 0.89f, 1.09f })
            {
                const auto a = render(sr, f0, ratio, 32);
                const auto b = render(sr, f0, ratio, 511);
                double diff = 0.0;
                for (size_t i = 0; i < a.output.size(); ++i)
                {
                    if (!std::isfinite(a.output[i]) || !std::isfinite(b.output[i]))
                        return 2;
                    diff = std::max(diff, std::abs(static_cast<double>(a.output[i]) - b.output[i]));
                }
                const bool pass = a.latency == b.latency && diff < 2.0e-4
                    && a.coverage > 0.88f && b.coverage > 0.88f
                    && a.contributors >= 1.5f && b.contributors >= 1.5f;
                std::printf("predictive grains %.0f Hz @ %.0f Hz, ratio %.2f: cov %.3f/%.3f contributors %.2f/%.2f maxdiff %.7f %s\n",
                    f0, sr, ratio, a.coverage, b.coverage, a.contributors, b.contributors,
                    diff, pass ? "PASS" : "FAIL");
                success = pass && success;
            }
    return success ? 0 : 1;
}
