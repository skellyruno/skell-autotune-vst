#include "SkeletonMotion.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    using namespace skellytune::motion;

    const auto paused = headPose(.95f, .85f, .94, true);
    assert(paused.x == 0 && paused.y == 0 && paused.angle == 0);
    const auto idle = headPose(0.0f, 0.0f, 1.7, false);
    assert(idle.x == 0 && idle.y == 0 && idle.angle == 0);

    const auto high = headPose(.80f, .75f, .94, false);
    const auto low = headPose(.80f, .75f, 2.06, false);
    assert(std::abs(high.y - low.y) > 4.0f); // clearly visible vertical bounce
    assert(std::abs(high.x - low.x) > .6f); // independent horizontal sway
    assert(std::abs(high.angle - low.angle) > .005f); // head tilt
    for (int i = 0; i <= 256; ++i) {
        const auto a = headPose(1.0f, 1.0f, .1 * i, false);
        assert(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.angle));
        assert(std::abs(a.x) <= 8.0f && a.y >= -17.0f && a.y <= 3.0f);
        assert(std::abs(a.angle) <= .11f);
    }

    const auto reduced = boneScatter(113, 97, 102, 139, 2.0f, 1, 1, 1.25, true);
    assert(reduced.x == 0 && reduced.y == 0);
    const auto silent = boneScatter(113, 97, 102, 139, 2.0f, 0, 1, 1.25, false);
    assert(silent.x == 0 && silent.y == 0);
    float maxMotion = 0;
    for (int i = 0; i <= 500; ++i) {
        const auto moving = boneScatter(113, 97, 102, 139, 2.0f,
                                        0.85f, 1.0f, .06 * i, false);
        assert(std::isfinite(moving.x) && std::isfinite(moving.y));
        assert(std::abs(moving.x) <= 13.0f && std::abs(moving.y) <= 13.0f);
        maxMotion = std::max(maxMotion, std::hypot(moving.x, moving.y));
        const auto detail = boneScatter(137, 81, 143, 83, 1.2f, 1, 1, .06*i, false);
        assert(std::abs(detail.x) <= 8.0f && std::abs(detail.y) <= 8.0f);
    }
    assert(maxMotion > 6.0f); // regression: old 2.5px lock is not allowed
    std::printf("PASS visible scatter peak %.2fpx and independent head bounce\n", maxMotion);
    return 0;
}
