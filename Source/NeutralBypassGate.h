#pragma once

#include <algorithm>
#include <cmath>

namespace humtune
{
// Sustain a transparent, latency-aligned dry path only after near-unity tuning
// has persisted. A broader exit threshold avoids a new crossfade on every
// small detector fluctuation about zero cents. Not used for unvoiced routing.
class NeutralBypassGate
{
public:
    void reset() noexcept { latched = false; stableSeconds = 0.0; }

    bool update (float appliedRatio, double hopSeconds, bool independentFormantShift) noexcept
    {
        if (independentFormantShift || !std::isfinite(appliedRatio)
            || appliedRatio <= 0.0f || !std::isfinite(hopSeconds) || hopSeconds <= 0.0)
        {
            reset();
            return false;
        }
        const double cents = std::abs(1200.0 * std::log2(static_cast<double>(appliedRatio)));
        if (cents <= 1.4)
            stableSeconds = std::min(0.1, stableSeconds + hopSeconds);
        else
            stableSeconds = 0.0;

        if (!latched && stableSeconds >= 0.008)
            latched = true;
        if (latched && cents >= 3.0)
            latched = false;
        return latched;
    }

    bool isLatched() const noexcept { return latched; }

private:
    double stableSeconds = 0.0;
    bool latched = false;
};
} // namespace humtune
