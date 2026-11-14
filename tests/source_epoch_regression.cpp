#include "PitchShifter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef NDEBUG
#error "Release regression assertions must be active"
#endif

// Bias the detector period while keeping the output grain clock at the requested
// target. Independently measure the rendered fundamental phase over Hann windows.
// This reproduces drift caused by integrating a slightly wrong source period.
static double measure(double sr, bool tracking, double bias, bool preserve, double target)
{
    constexpr double pi = 3.141592653589793;
    constexpr int block = 127;
    humtune::PitchShifter shifter;
    shifter.prepare(sr, block);
    shifter.setMinFrequency(80);
    shifter.setFormantPreserve(preserve);
    shifter.setPhaseLockedSource(true);
#ifndef SKELLYTUNE_TEST_NO_EPOCH_API
    shifter.setSourceEpochTrackingEnabled(tracking);
#endif
    const int count = static_cast<int>(sr * 0.9);
    std::vector<float> audio(static_cast<size_t>(count));
    double phase = 0;
    const double inputHz = 211.3, estimateHz = inputHz * std::exp2(bias / 1200.0);
    for (int i = 0; i < count; ++i)
    {
        phase += 2*pi*inputHz / sr;
        audio[static_cast<size_t>(i)] = static_cast<float>(
            0.2*std::sin(phase) + 0.07*std::sin(2*phase + 0.3)
            + 0.03*std::sin(3*phase + 0.5));
    }
    for (int i = 0; i < count; i += block)
        shifter.process(audio.data() + i, std::min(block, count - i),
                        static_cast<float>(sr / estimateHz),
                        static_cast<float>(target / estimateHz));

    double previousPhase = 0;
    bool havePrevious = false;
    std::vector<double> errors;
    const int step = static_cast<int>(sr * 0.02), half = static_cast<int>(sr * 0.025);
    for (int center = static_cast<int>(sr * 0.3); center < static_cast<int>(sr * 0.8); center += step)
    {
        double re = 0, im = 0;
        for (int j = -half; j <= half; ++j)
        {
            const double w = 0.5 + 0.5*std::cos(pi*j / half);
            const double angle = 2*pi*target*(center + j) / sr;
            const float x = audio[static_cast<size_t>(center + j)];
            assert(std::isfinite(x));
            re += w*x*std::cos(angle);
            im -= w*x*std::sin(angle);
        }
        const double p = std::atan2(im, re);
        if (havePrevious)
        {
            const double delta = std::remainder(p - previousPhase, 2*pi);
            const double frequency = target + delta / (2*pi*(step / sr));
            errors.push_back(std::abs(1200*std::log2(frequency / target)));
        }
        previousPhase = p;
        havePrevious = true;
    }
    std::sort(errors.begin(), errors.end());
    return errors[static_cast<size_t>(0.95*(errors.size() - 1))];
}

int main()
{
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (bool preserve : {false, true})
            for (double target : {196.0, 220.0, 246.94})
                for (double bias : {-30.0, 30.0})
                {
                    const double baseline = measure(sr, false, bias, preserve, target);
                    const double candidate = measure(sr, true, bias, preserve, target);
                    std::printf("sr %.0f preserve %d target %.2f bias %.0f: p95 %.3f -> %.3f cents\n",
                                sr, preserve, target, bias, baseline, candidate);
                    assert(candidate < 10.0);
                    assert(candidate < baseline * 0.45);
                }
}
