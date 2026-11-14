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
    // Motion thresholds were tuned for one estimate per 4 ms. With faster estimates, deltas are
    // still taken over that same 4 ms (stride = estimates per 4 ms); at stride 1 nothing changes.
    void setReferenceStride (int stride) noexcept { refStride = std::clamp(stride, 1, kMaxStride); }

    void reset() noexcept
    {
        hasPrevious = false;
        previousObserved = 0.0f;
        previousStep = 0.0f;
        historyCount = 0;
        lastTarget = -1.0f;
        uncertainSeconds = 0.0;
    }

    float choose(float observed, float predicted, float measuredTarget,
                 float predictedTarget, float confidence, double hopSeconds,
                 bool noteStabilizerActive, bool recentTrustedOnset = false,
                 bool allowForecast = true) noexcept
    {
        if (!std::isfinite(observed) || !std::isfinite(predicted)
            || !std::isfinite(measuredTarget) || !std::isfinite(predictedTarget)
            || !std::isfinite(confidence) || hopSeconds <= 0.0)
            return measuredTarget;

        history[static_cast<size_t>(historyCount % kHistory)] = observed;
        auto back = [&](int n) { return history[static_cast<size_t>(((historyCount - n) % kHistory + kHistory) % kHistory)]; };
        const bool haveStride = historyCount >= refStride;
        const float strideDelta = haveStride ? observed - back(refStride) : 0.0f;
        const float strideStep = historyCount >= 2 * refStride ? back(refStride) - back(2 * refStride) : 0.0f;
        ++historyCount;
        const bool hasPrevious = haveStride;
        const float delta = strideDelta;
        const float previousStep = strideStep;
        const int direction = delta > 0.025f ? 1 : (delta < -0.025f ? -1 : 0);
        const bool consistentMotion = hasPrevious && direction != 0 && direction == (previousStep > 0.025f ? 1 : (previousStep < -0.025f ? -1 : 0));
        // The wider onset shortcut extrapolates a historical contour. When
        // that contour is already braking, retain the ordinary narrow crossing
        // rule instead of inserting an unsupported one-hop upper/lower target.
        const bool brakingMotion = consistentMotion
            && std::abs(delta) < 0.85f * std::abs(previousStep);

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
            // A credible attack can substitute for the second consistent motion
            // observation, but cannot override weak confidence, a large leap,
            // or an uncertain predicted crossing. Unlike a fixed delay this
            // decision uses evidence already seen by the causal onset monitor.
            const bool onsetConfirms = recentTrustedOnset && hasPrevious && !brakingMotion
                && direction != 0 && confidence >= 0.975f
                && std::abs(delta) >= 0.06f
                && std::abs(observed - boundary) < 0.22f;
            if (allowForecast && (consistentMotion || onsetConfirms) && confidence >= 0.93f
                && (nearBoundary || onsetConfirms) && definitelyCrossing
                && std::abs(predictedMotion) <= 0.45f
                && std::abs(predictedTarget - measuredTarget) <= 2.1f)
                result = predictedTarget;
        }
        lastTarget = result;
        return result;
    }

private:
    static constexpr int kMaxStride = 4, kHistory = 2 * kMaxStride + 1;
    int refStride = 1, historyCount = 0;
    float history[kHistory] = {};
    bool hasPrevious = false;
    float previousObserved = 0.0f;
    float previousStep = 0.0f;
    float lastTarget = -1.0f;
    double uncertainSeconds = 0.0;
};
} // namespace humtune
