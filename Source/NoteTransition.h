#pragma once
#include <algorithm>
#include <cmath>

namespace humtune
{
// Small, allocation-free scale-target decision guard. It does not alter the
// detector or pitch prediction itself; it only prevents confidence-poor
// target jumps and allows a *confirmed* moving pitch to cross a scale boundary
// without an extra detector-hop delay. All time accounting uses hop seconds.
class NoteTransition
{
public:
    void reset() noexcept
    {
        hasPrevious = false;
        previousObserved = 0.0f;
        previousDirection = 0;
        lastTarget = -1.0f;
        uncertainSeconds = 0.0;
    }

    float choose(float observed, float predicted, float measuredTarget,
                 float predictedTarget, float confidence, double hopSeconds,
                 bool noteStabilizerActive) noexcept
    {
        if (!std::isfinite(observed) || !std::isfinite(predicted)
            || !std::isfinite(measuredTarget) || !std::isfinite(predictedTarget)
            || !std::isfinite(confidence) || hopSeconds <= 0.0)
            return measuredTarget;

        const float delta = hasPrevious ? observed - previousObserved : 0.0f;
        const int direction = delta > 0.025f ? 1 : (delta < -0.025f ? -1 : 0);
        const bool consistentMotion = hasPrevious && direction != 0 && direction == previousDirection;
        previousDirection = direction;
        previousObserved = observed;
        hasPrevious = true;

        float result = measuredTarget;
        if (!noteStabilizerActive)
        {
            if (confidence < 0.62f && lastTarget >= 0.0f)
            {
                uncertainSeconds += hopSeconds;
                // Brief low-confidence consonants / octave glitches shouldn't
                // re-target the voice. Don't remain stuck beyond a 12-ms gap.
                if (uncertainSeconds < 0.012)
                    return lastTarget;
            }
            else
                uncertainSeconds = 0.0;

            const float predictedMotion = predicted - observed;
            const float boundary = 0.5f * (measuredTarget + predictedTarget);
            const bool nearBoundary = std::abs(observed - boundary) < 0.15f;
            const bool definitelyCrossing = (predictedTarget > measuredTarget
                 && predicted > boundary + 0.10f && predictedMotion > 0.05f)
                || (predictedTarget < measuredTarget
                 && predicted < boundary - 0.10f && predictedMotion < -0.05f);
            if (consistentMotion && confidence >= 0.93f && nearBoundary
                && definitelyCrossing && std::abs(predictedMotion) <= 0.45f
                && std::abs(predictedTarget - measuredTarget) <= 2.1f)
                result = predictedTarget;
        }
        lastTarget = result;
        return result;
    }

private:
    bool hasPrevious = false;
    float previousObserved = 0.0f;
    int previousDirection = 0;
    float lastTarget = -1.0f;
    double uncertainSeconds = 0.0;
};
} // namespace humtune
