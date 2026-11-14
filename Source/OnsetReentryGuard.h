#pragma once
#include <algorithm>
#include <cmath>

namespace humtune
{
// Causal control evidence only: no audio buffering or pitch persistence.
// A remembered attack belongs to a dropout, not a sustained voiced pulse.
class OnsetReentryGuard
{
public:
    void reset() noexcept { remainingSeconds = 0.0; }
    void advance(bool onset, bool duringDropout, double hopSeconds) noexcept
    {
        remainingSeconds = std::max(0.0, remainingSeconds - hopSeconds);
        if (onset && duringDropout)
            remainingSeconds = 0.032;
    }
    bool hasTrustedOnset(float confidence) const noexcept
    {
        return remainingSeconds > 0.0 && std::isfinite(confidence) && confidence >= 0.85f;
    }
    // twoCycleThreshold is 0.50 at the normal hop; the optional fast hop passes a higher value because
    // its first estimate is taken from ~2 ms less recorded history and a borderline fit can pass by timing.
    static bool acceptsFirstPitch(float confidence, float twoCycleCorrelation, float oneCycleCorrelation = -1.0f,
                                  float twoCycleThreshold = 0.50f) noexcept
    {
        return std::isfinite(confidence) && std::isfinite(twoCycleCorrelation)
            && (confidence >= 0.94f || twoCycleCorrelation == -1.0f || twoCycleCorrelation >= twoCycleThreshold
                || (twoCycleCorrelation >= 0.30f && std::isfinite(oneCycleCorrelation)
                    && oneCycleCorrelation >= 0.90f));
    }
private:
    double remainingSeconds = 0.0;
};
}
