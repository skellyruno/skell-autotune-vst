#include "AutoTuneEngine.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

// Covers hostile but legal parameter automation and malformed floating-point
// samples. UI telemetry is read simultaneously with DSP to exercise its atomic
// publication path. This is a smoke/regression test, not an audio quality score.
int main()
{
    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        humtune::AutoTuneEngine engine;
        engine.prepare(sampleRate, 256);
        engine.setRootNote(-25);
        engine.setReferenceFreq(std::numeric_limits<float>::quiet_NaN());
        engine.setRetuneSpeed(std::numeric_limits<float>::infinity());
        engine.setHumanize(std::numeric_limits<float>::quiet_NaN());
        engine.setAmount(std::numeric_limits<float>::quiet_NaN());
        engine.setSustain(std::numeric_limits<float>::quiet_NaN());
        engine.setGlideMilliseconds(std::numeric_limits<float>::infinity());
        engine.setFormantShiftSemitones(std::numeric_limits<float>::quiet_NaN());
        engine.setFlexTune(std::numeric_limits<float>::quiet_NaN());

        std::atomic<bool> stop{false};
        std::atomic<bool> readFailed{false};
        std::thread reader([&]
        {
            while (!stop.load(std::memory_order_acquire))
            {
                if (!std::isfinite(engine.getDetectedHz()) ||
                    !std::isfinite(engine.getTargetHz()) ||
                    !std::isfinite(engine.getCorrectionCents()) ||
                    !std::isfinite(engine.getConfidence()) ||
                    !std::isfinite(engine.getTargetMidiFloat()))
                    readFailed.store(true, std::memory_order_relaxed);
                const auto key = engine.getAutoKeyEstimate();
                if (!std::isfinite(key.confidence) || !std::isfinite(key.evidenceSeconds))
                    readFailed.store(true, std::memory_order_relaxed);
            }
        });

        double phase = 0.0;
        int bad = 0;
        for (int iter = 0; iter < 160; ++iter)
        {
            const int length = iter % 3 == 0 ? 17 : (iter % 3 == 1 ? 127 : 512);
            juce::AudioBuffer<float> audio(2, length);
            for (int i = 0; i < length; ++i)
            {
                phase += 2.0 * 3.14159265358979323846 * 220.0 / sampleRate;
                const float x = 0.25f * std::sin(phase);
                audio.getWritePointer(0)[i] = x;
                audio.getWritePointer(1)[i] = -x; // anti-phase detector downmix
            }
            if (iter % 9 == 0)
                audio.getWritePointer(0)[length / 2] = std::numeric_limits<float>::quiet_NaN();
            if (iter % 11 == 0)
                audio.getWritePointer(1)[length / 3] = std::numeric_limits<float>::infinity();

            if (iter % 19 == 0)
                engine.setVoiceRange((iter / 19) % humtune::AutoTuneEngine::kNumRanges);
            if (iter % 23 == 0)
                engine.setLowLatencyMode((iter / 23) % 2 != 0);
            if (iter % 29 == 0)
                engine.setEnabled((iter / 29) % 2 == 0);
            engine.process(audio);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < length; ++i)
                    if (!std::isfinite(audio.getReadPointer(ch)[i]))
                        ++bad;
        }
        stop.store(true, std::memory_order_release);
        reader.join();
        if (bad != 0 || readFailed.load(std::memory_order_relaxed))
        {
            std::fprintf(stderr, "Engine safety regression failed (sr %.0f, invalid output %d)\n", sampleRate, bad);
            return 1;
        }
    }
    std::puts("engine safety regression passed");
    return 0;
}
