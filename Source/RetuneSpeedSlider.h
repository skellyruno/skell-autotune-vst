#pragma once

#include <JuceHeader.h>

namespace humtune
{
// Retune is stored as 0=fast, 1=slow for existing presets and automation.
// The editor labels the same control as SPEED, so clockwise/right drag must
// visibly increase FASTNESS without changing the historical host parameter.
class RetuneSpeedSlider final : public juce::Slider
{
public:
    double proportionOfLengthToValue(double proportion) override
    {
        return juce::Slider::proportionOfLengthToValue(1.0 - proportion);
    }

    double valueToProportionOfLength(double value) override
    {
        return 1.0 - juce::Slider::valueToProportionOfLength(value);
    }
};
} // namespace humtune
