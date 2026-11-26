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
    void setReferenceStride (int stride) noexcept
    {
        const int nextStride = std::clamp(stride, 1, kMaxStride);
        if (nextStride != refStride)
        {
            // Do not mix observations spaced 2 ms and 4 ms in one velocity
            // history. Keep the accepted target through the mode switch.
            refStride = nextStride;
            historyCount = 0;
            historyWriteIndex = 0;
        }
    }

    void reset() noexcept
    {
        historyCount = 0;
        historyWriteIndex = 0;
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
            || !std::isfinite(confidence) || !std::isfinite(hopSeconds)
            || hopSeconds <= 0.0)
        {
            // Invalid detector readings must not commit an unverified note.
            // The prior early return leaked measuredTarget even if different
            // from the last accepted note, causing a one-hop pitch jump.
            historyCount = 0;
            historyWriteIndex = 0;
            uncertainSeconds = 0.0;
            if (std::isfinite(lastTarget) && lastTarget >= 0.0f)
                return lastTarget;
            return std::isfinite(measuredTarget) ? measuredTarget : 69.0f;
        }

        // Predictive crossings require a trajectory of TRUSTED detector
        // observations. A noisy weak-confidence ramp must not make the
        // next credible frame appear to have confirmed velocity. Reset only
        // the motion evidence, not lastTarget/its brief confidence hold.
        // A bounded ring counter also avoids signed overflow in long sessions.
        constexpr float kMotionTrust = 0.93f; // matches forecast confidence gate
        float delta = 0.0f, previousStep = 0.0f;
        bool hasPrevious = false;
        if (confidence >= kMotionTrust)
        {
            const int index = historyWriteIndex;
            history[static_cast<size_t>(index)] = observed;
            const auto back = [&](int n) {
                return history[static_cast<size_t>(
                    (index + kHistory - n) % kHistory)];
            };
            hasPrevious = historyCount >= refStride;
            if (hasPrevious)
                delta = observed - back(refStride);
            if (historyCount >= 2 * refStride)
                previousStep = back(refStride) - back(2 * refStride);
            historyCount = std::min(historyCount + 1, kHistory);
            historyWriteIndex = (index + 1) % kHistory;
        }
        else
        {
            historyCount = 0;
            historyWriteIndex = 0;
        }
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
    int refStride = 1, historyCount = 0, historyWriteIndex = 0;
    float history[kHistory] = {};
    float lastTarget = -1.0f;
    double uncertainSeconds = 0.0;
};
} // namespace humtune
