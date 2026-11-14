#include "KeyEstimator.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
    humtune::KeyEstimator estimator;
    // Long gaps must expire evidence rather than decaying only one second.
    for (int i=0; i<120; ++i) {
        estimator.advanceTime(0.05);
        estimator.observePitch(static_cast<float>((i%3)*4+60), 1.0f, 0.05);
    }
    const auto before=estimator.estimate();
    assert(before.evidenceSeconds > 1.5f);
    estimator.advanceTime(180.0);
    assert(estimator.estimate().evidenceSeconds < 0.01f);
    // Very large finite pitch values must not overflow integer conversion.
    estimator.observePitch(1.0e30f, 1.0f, 0.05);
    estimator.observePitch(-1.0e30f, 1.0f, 0.05);
    assert(std::isfinite(estimator.estimate().evidenceSeconds));
    std::cout << "key estimator regression passed\n";
}
