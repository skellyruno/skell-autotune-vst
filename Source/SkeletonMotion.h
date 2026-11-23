#pragma once

#include <algorithm>
#include <cmath>

// Pure, deterministic UI animation curves. No audio-thread work, allocations,
// random state, hidden clocks or new plugin parameters. Shared by the real
// JUCE renderer and an offline regression to preserve the user's animation.
namespace skellytune::motion {

struct HeadPose { float x = 0.0f, y = 0.0f, angle = 0.0f; };
struct BoneScatter { float x = 0.0f, y = 0.0f; };

inline HeadPose headPose(float activity, float motionLevel,
                         double phase, bool reduceMotion) noexcept
{
    if (reduceMotion) return {};
    const float active = std::clamp(activity, 0.0f, 1.0f);
    const float motion = std::clamp(motionLevel, 0.0f, 1.0f);
    const float drive = std::max(active, motion);
    const float p = static_cast<float>(phase);
    // Distinct vertical bounce, side sway and head tilt; unlike a single
    // whole-body rotation this keeps the skull as an animated focal point.
    const float lift = std::pow(std::abs(std::sin(1.54f * p)), 1.6f);
    const float x = drive * ((2.7f + 4.2f * active) * std::sin(.94f * p)
                           + 1.35f * std::sin(2.18f * p));
    const float y = drive * (-(5.2f + 10.4f * active) * lift
                           + 1.8f * std::sin(.72f * p));
    const float angle = drive * (.025f + .075f * active)
                       * std::sin(1.12f * p + .28f);
    return {std::clamp(x, -8.0f, 8.0f),
            std::clamp(y, -17.0f, 3.0f),
            std::clamp(angle, -.11f, .11f)};
}

inline BoneScatter boneScatter(float x, float y, float xx, float yy,
                               float width, float activity, float intensity,
                               double phase, bool reduceMotion) noexcept
{
    if (reduceMotion) return {};
    const float a = std::clamp(activity, 0.0f, 1.0f);
    const float power = std::clamp(intensity, 0.0f, 1.5f);
    const float p = static_cast<float>(phase);
    const float identity = x * .17f + y * .071f + xx * .023f + yy * .031f;
    const float pulse = .66f + .34f * std::abs(
        std::sin(p * (.72f + .08f * std::sin(x)) + identity * .61f));
    // Strong, VISIBLE scatter: up to 13px for major arm/leg strokes,
    // 8px for finger/vertebra details. Complete strokes move together,
    // avoiding the old 30-50px explosive break-apart.
    const float limit = width > 1.5f ? 13.0f : 8.0f;
    const float displacement = std::min(limit, limit * a * power * pulse);
    return {displacement * std::sin(p * 1.13f + identity),
            .76f * displacement * std::cos(p * .87f + identity * 1.37f)};
}
} // namespace skellytune::motion
