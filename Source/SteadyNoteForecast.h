#pragma once
#include <algorithm>
#include <cmath>

namespace humtune
{
// Separates causal note *decisions* from the synthesis period estimate.
// A transient still uses the fully predicted input F0; only a stable,
// reliable note permits 54%-maximum down-weighting of historical velocity.
// This avoids extrapolating YIN window lag/jitter indefinitely on held vowels.
// No allocation, buffering, locks, or noncausal samples are involved.
class SteadyNoteForecast
{
public:
    void reset() noexcept
    {
        lastTarget = -100.0f;
        heldSeconds = 0.0;
    }

    float process(float observedMidi, float predictedMidi, float targetMidi,
                  float confidence, double hopSeconds, bool lowLatency,
                  bool recentOnset) noexcept
    {
        if (!std::isfinite(observedMidi) || !std::isfinite(predictedMidi)
            || !std::isfinite(targetMidi) || !std::isfinite(confidence)
            || !std::isfinite(hopSeconds) || hopSeconds <= 0.0)
        {
            reset();
            return std::isfinite(observedMidi) ? observedMidi
                : (std::isfinite(predictedMidi) ? predictedMidi
                   : (std::isfinite(targetMidi) ? targetMidi : 69.0f));
        }
        if (std::abs(targetMidi - lastTarget) < 0.01f)
            heldSeconds = std::min(1.0, heldSeconds + hopSeconds);
        else
        {
            lastTarget = targetMidi;
            heldSeconds = 0.0;
        }
        const bool reliableHold = confidence >= 0.85f
            && std::abs(observedMidi - targetMidi) < 0.65f && !recentOnset;
        const double steady = reliableHold && !lowLatency
            ? std::clamp((heldSeconds - 0.024) / 0.024, 0.0, 1.0) : 0.0;
        const float predictionWeight = static_cast<float>(1.0 - 0.54 * steady);
        return observedMidi + predictionWeight * (predictedMidi - observedMidi);
    }

private:
    float lastTarget = -100.0f;
    double heldSeconds = 0.0;
};
} // namespace humtune
