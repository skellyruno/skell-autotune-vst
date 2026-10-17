#pragma once

#include <algorithm>
#include <cmath>

namespace humtune
{

// Slews a quantized target independently of Retune Speed. Zero milliseconds is an exact
// passthrough to the target; larger values create musical portamento between note targets.
// No allocation, locking, or variable-length state is used.
class TargetGlide
{
public:
    void reset() noexcept { initialized = false; current = 0.0f; }

    float process(float targetMidi, float glideMilliseconds, double deltaSeconds) noexcept
    {
        if (!std::isfinite(targetMidi))
            return current;
        // Reject implausible MIDI values before any interpolation. They can arise
        // from transient detector failures or corrupted automation and should
        // never drag the glide state far outside the musical note range.
        targetMidi = std::clamp(targetMidi, 0.0f, 127.0f);
        if (!initialized)
        {
            initialized = true;
            current = targetMidi;
            return current;
        }

        const float glideMs = std::clamp(std::isfinite(glideMilliseconds) ? glideMilliseconds : 0.0f,
                                         0.0f, 500.0f);
        // Preserve the previous engine path exactly when glide is at its default value.
        if (glideMs <= 0.01f || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0)
        {
            current = targetMidi;
            return current;
        }

        // 95% of the target distance is covered in about the chosen Glide time.
        const double timeConstant = static_cast<double>(glideMs) / 3000.0;
        // Bound the step duration after a transport pause or a host stall.
        // Without this, a single unusually large delta bypasses the audible
        // glide and produces an unexpected jump at the next note boundary.
        const double boundedDelta = std::min(deltaSeconds, 0.050);
        const double coefficient = 1.0 - std::exp(-boundedDelta / timeConstant);
        current += static_cast<float>(coefficient) * (targetMidi - current);
        if (std::abs(current - targetMidi) < 0.001f)
            current = targetMidi;
        return current;
    }

    bool hasTarget() const noexcept { return initialized; }
    float value() const noexcept { return current; }

private:
    bool initialized = false;
    float current = 0.0f;
};

} // namespace humtune
