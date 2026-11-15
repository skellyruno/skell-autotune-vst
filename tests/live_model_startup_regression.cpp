#include "PitchShifter.h"
#include <cassert>
#include <cmath>
#include <vector>

int main()
{
    // A low note needs two observed cycles before the live model exists.
    // Until then, output must be the input delayed by the reported PDC.
    for (double rate : {44100.0, 48000.0, 96000.0})
    for (int block : {1, 127, 512})
    {
        humtune::PitchShifter shifter;
        shifter.prepare(rate, block);
        shifter.setMinFrequency(80);
        shifter.setLowLatencyExperiment(true);
        shifter.setLowLatencyBudgetMilliseconds(1.9);
        shifter.setPhaseLockedSource(true);
        shifter.setFormantPreserve(false);
        const auto delay = shifter.getLatencySamples();
        const auto count = static_cast<int>(rate * .018);
        std::vector<float> input(static_cast<size_t>(count));
        for (int n = 0; n < count; ++n)
            input[static_cast<size_t>(n)] = static_cast<float>(.18 * std::sin(2 * juce::MathConstants<double>::pi * 80 * n / rate));
        auto output = input;
        for (int n = 0; n < count; n += block)
            shifter.process(output.data() + n, std::min(block, count - n), rate / 80.0, 1.06);
        for (int n = delay; n < count; ++n)
            assert(output[static_cast<size_t>(n)] == input[static_cast<size_t>(n - delay)]);
    }
}
