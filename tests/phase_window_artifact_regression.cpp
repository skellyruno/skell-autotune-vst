#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

// Regression for the v32 phase-history change. Repeated vowels separated by
// aperiodic consonants and abrupt note/ratio changes must not depend on DAW
// buffer size, produce NaNs, or alter the plugin's published audio delay.
static std::vector<float> render(double sr, int block, bool low, int& delay)
{
    humtune::PitchShifter shifter;
    shifter.prepare(sr, block);
    shifter.setMinFrequency(100.0);
    shifter.setLowLatencyExperiment(low);
    if (low) shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setPhaseLockedSource(true);
    shifter.setFormantPreserve(false);
    delay = shifter.getLatencySamples();
    const int size = static_cast<int>(sr * 0.62);
    const int boundary[5] = {0, size / 5, 2 * size / 5, 3 * size / 5, 4 * size / 5};
    std::vector<float> audio(static_cast<size_t>(size));
    double phi = 0;
    uint32_t rng = 0x1345726u;
    for (int i=0;i<size;++i)
    {
        const int section = static_cast<int>(std::upper_bound(boundary, boundary + 5, i) - boundary) - 1;
        const double hz = (section >= 3 ? 293.66 : 155.56);
        phi += 6.283185307179586 * hz / sr;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const float noise = static_cast<float>((rng / 4294967296.0) * 2.0 - 1.0);
        audio[static_cast<size_t>(i)] = section == 2 ? 0.10f * noise :
            static_cast<float>(0.22 * (std::sin(phi) + 0.37 * std::sin(2 * phi + .15)
                + .21 * std::sin(3 * phi + .42)));
    }
    for (int i=0;i<size;)
    {
        const int section=static_cast<int>(std::upper_bound(boundary, boundary + 5, i) - boundary) - 1;
        const int next=(section==4 ? size : boundary[section + 1]);
        const int count=std::min(block, next - i);
        if (count <= 0) return {};
        const float hz = section >= 3 ? 293.66f : 155.56f;
        shifter.setDryPassthrough(section == 2);
        shifter.process(audio.data() + i, count, static_cast<float>(sr / hz),
                        section >= 3 ? 1.12f : 0.89f);
        const auto diag = shifter.getGrainDiagnostics();
        if (!std::isfinite(diag.phaseMatchScore) || !std::isfinite(diag.maxOutputStep)
            || diag.maxOutputStep > 1.8f || !std::isfinite(diag.appliedGain)
            || diag.appliedGain > 4.1f)
            return {};
        i+=count;
    }
    return audio;
}
int main()
{
    for (double sr : {44100.0, 48000.0, 96000.0})
      for (bool low : {false, true})
      {
        int aDelay=0,bDelay=0;
        const auto a=render(sr, 47, low, aDelay);
        const auto b=render(sr, 511, low, bDelay);
        if (a.empty() || b.size()!=a.size() || aDelay!=bDelay)
            return 2;
        double maxDiff=0.0, outputEnergy=0.0;
        for (size_t i=0;i<a.size();++i)
        {
            if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return 3;
            maxDiff=std::max(maxDiff, std::abs(static_cast<double>(a[i]-b[i])));
            outputEnergy+=static_cast<double>(a[i])*a[i];
        }
        std::printf("phase-window sr=%.0f low=%d samples=%zu maxDiff=%.9g energy=%.4g delay=%d\n",
                    sr, static_cast<int>(low), a.size(), maxDiff, outputEnergy, aDelay);
        if (maxDiff > 1.0e-5 || outputEnergy < 0.001)
            return 1;
      }
    return 0;
}
