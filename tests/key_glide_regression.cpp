#include "AutoTuneEngine.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int pcFor(int root, int degree) { return (root + degree) % 12; }

void feedScale(humtune::KeyEstimator& estimator, int root, bool minor)
{
    static constexpr std::array<int, 7> majorDegrees{0, 2, 4, 5, 7, 9, 11};
    static constexpr std::array<int, 7> minorDegrees{0, 2, 3, 5, 7, 8, 10};
    const auto& degrees = minor ? minorDegrees : majorDegrees;
    const auto& profile = minor ? humtune::KeyEstimator::kMinorProfile
                                : humtune::KeyEstimator::kMajorProfile;
    // Simulate a vocal phrase whose stable pitch-class durations follow a plausible
    // tonal profile. Small time steps exercise the same continuously-decaying histogram
    // used from the engine's detector-hop callback.
    for (int repeat = 0; repeat < 10; ++repeat)
    {
        for (int degree : degrees)
        {
            const int pc = pcFor(root, degree);
            const double duration = std::clamp(static_cast<double>(profile[static_cast<std::size_t>(degree)]) * 0.012,
                                               0.015, 0.08);
            const int ticks = static_cast<int>(std::ceil(duration / 0.004));
            const double dt = duration / ticks;
            for (int t = 0; t < ticks; ++t)
            {
                estimator.advanceTime(dt);
                estimator.observePitch(static_cast<float>(60 + pc), 0.96f, dt);
            }
        }
    }
}

float renderEngineTargetAfterJump(float glideMs)
{
    constexpr double sr = 44100.0;
    constexpr int blockSize = 127;
    constexpr int totalSamples = static_cast<int>(1.3 * sr);
    constexpr int transitionSample = static_cast<int>(0.7 * sr);
    constexpr int measureSample = static_cast<int>(1.0 * sr);

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(3);
    engine.setRootNote(0);
    engine.setScaleType(0); // C major; C and G are both legal targets.
    engine.setRetuneSpeed(0.0f);
    engine.setNoteStabilizer(false);
    engine.setGlideMilliseconds(glideMs);
    engine.setLowLatencyMode(true);
    engine.prepare(sr, blockSize);

    double phase = 0.0;
    int offset = 0;
    bool captured = false;
    float targetAtMeasure = -1.0f;
    while (offset < totalSamples)
    {
        const int n = std::min(blockSize, totalSamples - offset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const int sample = offset + i;
            const double hz = sample < transitionSample ? 261.625565 : 391.995436;
            phase += 2.0 * 3.14159265358979323846 * hz / sr;
            block.getWritePointer(0)[i] = static_cast<float>(0.25 * std::sin(phase));
        }
        engine.process(block);
        offset += n;
        if (!captured && offset >= measureSample)
        {
            targetAtMeasure = engine.getTargetMidiFloat();
            captured = true;
        }
    }
    return targetAtMeasure;
}

bool runEngineAutoKeyIntegration()
{
    constexpr double sr = 44100.0;
    constexpr int blockSize = 127;
    constexpr int pcs[] = {0, 7, 4, 9, 2, 5, 11}; // C, G, E, A, D, F, B
    constexpr double durations[] = {0.254, 0.208, 0.139, 0.146, 0.089, 0.093, 0.115};
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(3);
    engine.setRootNote(0);
    engine.setScaleType(2); // chromatic: don't bias detection toward the output scale
    engine.setRetuneSpeed(0.0f);
    engine.setNoteStabilizer(false);
    engine.setLowLatencyMode(true);
    engine.prepare(sr, blockSize);

    double phase = 0.0;
    for (int repeat = 0; repeat < 8; ++repeat)
    {
        for (int note = 0; note < 7; ++note)
        {
            const int segmentLength = static_cast<int>(durations[note] * sr);
            const double midi = 60.0 + pcs[note];
            const double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
            for (int position = 0; position < segmentLength;)
            {
                const int n = std::min(blockSize, segmentLength - position);
                juce::AudioBuffer<float> block(1, n);
                for (int i = 0; i < n; ++i)
                {
                    const double t = static_cast<double>(position + i) / sr;
                    const double fade = std::min({1.0, t / 0.004,
                        (durations[note] - t) / 0.004});
                    phase += 2.0 * 3.14159265358979323846 * hz / sr;
                    const double voiced = std::sin(phase) + 0.45 * std::sin(2.0 * phase)
                                        + 0.20 * std::sin(3.0 * phase);
                    block.getWritePointer(0)[i] = static_cast<float>(0.16 * voiced * std::max(0.0, fade));
                }
                engine.process(block);
                position += n;
            }
        }
    }

    const auto estimate = engine.getAutoKeyEstimate();
    const bool pass = estimate.valid && estimate.root == 0 && !estimate.minor;
    std::printf("Auto Key engine integration: C major phrase -> root %d %s, confidence %.1f%%, evidence %.2fs %s\n",
                estimate.root, estimate.minor ? "minor" : "major", estimate.confidence * 100.0f,
                estimate.evidenceSeconds, pass ? "PASS" : "FAIL");
    return pass;
}

}

int main()
{
    {
        humtune::KeyEstimator estimator;
        check(!estimator.estimate().valid, "empty pitch history must not return a key");
        feedScale(estimator, 0, false);
        const auto e = estimator.estimate();
        check(e.valid, "sufficient C-major phrase should produce a key estimate");
        check(e.root == 0 && !e.minor, "major phrase should correctly estimate C major");
        check(e.confidence > 0.2f, "clear major phrase should have usable confidence");
    }
    {
        humtune::KeyEstimator estimator;
        feedScale(estimator, 9, true);
        const auto e = estimator.estimate();
        check(e.valid, "sufficient A-minor phrase should produce a key estimate");
        check(e.root == 9 && e.minor, "minor phrase should correctly estimate A minor");
    }
    {
        humtune::KeyEstimator estimator;
        for (int i = 0; i < 400; ++i)
        {
            const int midi = 60 + (i % 12);
            estimator.advanceTime(0.004);
            estimator.observePitch(static_cast<float>(midi), 0.96f, 0.004);
        }
        const auto chromatic = estimator.estimate();
        check(!chromatic.valid, "uniform chromatic input should not claim a confident key");
        for (int i = 0; i < 5000; ++i)
            estimator.advanceTime(0.004);
        check(!estimator.estimate().valid, "stale history should decay out after silence");
    }
    {
        humtune::TargetGlide glide;
        const float first = glide.process(60.0f, 300.0f, 0.004);
        check(std::abs(first - 60.0f) < 1.0e-6f, "glide starts directly on first voiced target");
        const float zeroGlide = glide.process(64.0f, 0.0f, 0.004);
        check(std::abs(zeroGlide - 64.0f) < 1.0e-6f, "zero glide must target immediately");
        glide.reset();
        check(glide.process(60.0f, 250.0f, 0.004) == 60.0f, "glide seeds without moving the first note");
        const float stalled = glide.process(72.0f, 250.0f, 1.0);
        check(stalled > 60.0f && stalled < 68.0f, "large transport delta must not snap glide to new note");
        check(glide.process(NAN, 250.0f, 0.004) == stalled, "invalid note leaves glide stable");
        check(glide.process(300.0f, 0.0f, 0.004) == 127.0f, "out-of-range MIDI is clamped");
        glide.reset();
        glide.process(60.0f, 300.0f, 0.004);
        const float next = glide.process(64.0f, 300.0f, 0.004);
        check(next > 60.0f && next < 64.0f, "positive glide must move smoothly toward new target");
        float prev = next;
        for (int i = 0; i < 500; ++i)
        {
            const float now = glide.process(64.0f, 300.0f, 0.004);
            check(now >= prev - 1.0e-6f, "ascending target glide must be monotonic");
            check(now <= 64.0f + 1.0e-6f, "target glide must not overshoot");
            prev = now;
        }
        check(std::abs(prev - 64.0f) < 0.002f, "glide should converge to target");
    }

    {
        const float immediate = renderEngineTargetAfterJump(0.0f);
        const float gradual = renderEngineTargetAfterJump(300.0f);
        check(std::abs(immediate - 67.0f) < 0.05f, "engine zero-glide target should switch directly to G");
        check(gradual > 62.5f && gradual < 66.95f,
              "engine positive glide should still be between C and G 300 ms after a transition");
        check(gradual < immediate - 0.2f, "engine glide must be distinct from Retune Speed");
    }
    check(runEngineAutoKeyIntegration(), "engine should estimate C major from a synthetic C-major vocal phrase");

    if (failures == 0)
        std::printf("Key + Glide regression: PASS (profile key estimator, live-engine key detection, target glide, ambiguity and decay)\n");
    return failures == 0 ? 0 : 1;
}
