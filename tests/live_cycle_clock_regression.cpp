#include "PitchShifter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef NDEBUG
#error "Release quality assertions must remain enabled"
#endif
namespace {
constexpr double pi = 3.141592653589793;
double sourceHz(double t)
{
    return 211.3 * std::exp2(0.65 * std::sin(2*pi*5.7*t) / 12.0);
}
std::vector<float> render(double sr, bool track, bool preserve, int block)
{
    humtune::PitchShifter s;
    s.prepare(sr, block);
    s.setMinFrequency(80);
    s.setLowLatencyExperiment(true);
    s.setLowLatencyBudgetMilliseconds(1.9);
    s.setFormantPreserve(preserve);
    s.setPhaseLockedSource(true);
#ifndef SKELLYTUNE_TEST_NO_EPOCH_API
    s.setSourceEpochTrackingEnabled(track);
#endif
    const int count = static_cast<int>(sr * 1.25);
    const int hop = static_cast<int>(sr * 0.004);
    std::vector<float> audio(static_cast<size_t>(count));
    double phase = 0;
    for (int i = 0; i < count; ++i)
    {
        phase += 2*pi*sourceHz(i/sr)/sr;
        audio[static_cast<size_t>(i)] = static_cast<float>(0.2*std::sin(phase)
            + 0.07*std::sin(2*phase+0.3) + 0.03*std::sin(3*phase+0.5));
    }
    for (int offset = 0; offset < count;)
    {
        const int n = std::min({block, count-offset, hop-offset%hop});
        // Model a historical detector observation. Target remains exactly 220 Hz:
        // source geometry must not perturb its independent synthesis clock.
        const double prior = sourceHz(std::max(0.0, (offset/hop*hop)/sr - 0.016));
        s.process(audio.data()+offset, n, static_cast<float>(sr/prior), static_cast<float>(220/prior));
        offset += n;
    }
    return audio;
}
double pitch95(const std::vector<float>& audio, double sr)
{
    const int step = static_cast<int>(sr*0.01), half = static_cast<int>(sr*0.025);
    double previous = 0;
    bool havePrevious = false;
    std::vector<double> error;
    for (int center = static_cast<int>(sr*0.3); center < static_cast<int>(sr*1.1); center += step)
    {
        double re = 0, im = 0;
        for (int j = -half; j <= half; ++j)
        {
            const double w = 0.5 + 0.5*std::cos(pi*j/half);
            const double angle = 2*pi*220*(center+j)/sr;
            const float x = audio[static_cast<size_t>(center+j)];
            assert(std::isfinite(x));
            re += w*x*std::cos(angle); im -= w*x*std::sin(angle);
        }
        const double phase = std::atan2(im,re);
        if (havePrevious)
        {
            const double delta = std::remainder(phase-previous,2*pi);
            const double frequency = 220 + delta/(2*pi*step/sr);
            error.push_back(std::abs(1200*std::log2(frequency/220)));
        }
        previous = phase; havePrevious = true;
    }
    std::sort(error.begin(),error.end());
    return error[static_cast<size_t>(0.95*(error.size()-1))];
}
}
int main()
{
    for (double sr : {44100.0,48000.0,96000.0})
        for (bool preserve : {false,true})
        {
            const double baseline = pitch95(render(sr,false,preserve,127),sr);
            const auto candidate = render(sr,true,preserve,127);
            const double after = pitch95(candidate,sr);
            std::printf("live cycle sr=%.0f formants=%d: p95 %.3f -> %.3f cents\n",sr,preserve,baseline,after);
            assert(after < (preserve ? 12.0 : 5.0));
            assert(after < baseline*0.3);
            for (int block : {1,512})
                assert(candidate == render(sr,true,preserve,block));
        }
}
