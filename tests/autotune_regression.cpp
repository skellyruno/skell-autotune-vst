#include "AutoTuneEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

struct DetectorCase { double hz; int range; int block; bool antiPhase; };

bool runTone(const DetectorCase& test)
{
    constexpr double sr = 48000.0;
    humtune::AutoTuneEngine engine;
    engine.prepare(sr, test.block);
    engine.setVoiceRange(test.range);
    engine.setEnabled(true);

    constexpr int total = 48000;
    int offset = 0;
    while (offset < total)
    {
        const int n = std::min(test.block, total - offset);
        juce::AudioBuffer<float> block(2, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            const float x = 0.3f * static_cast<float>(std::sin(2.0 * kPi * test.hz * t));
            block.getWritePointer(0)[i] = x;
            block.getWritePointer(1)[i] = test.antiPhase ? -x : x * 0.7f;
        }
        engine.process(block);
        offset += n;
    }

    const float detected = engine.getDetectedHz();
    const bool pass = detected > 0.0f && std::abs(detected - test.hz) < std::max(2.0, test.hz * 0.018);
    std::printf("detector: input %.2f Hz range %d block %d anti-phase %s -> %.2f Hz conf %.3f %s\n",
                test.hz, test.range, test.block, test.antiPhase ? "yes" : "no",
                detected, engine.getConfidence(), pass ? "PASS" : "FAIL");
    return pass;
}

bool runSilence()
{
    humtune::AutoTuneEngine engine;
    engine.prepare(48000.0, 127);
    engine.setEnabled(true);
    for (int blockIndex = 0; blockIndex < 100; ++blockIndex)
    {
        juce::AudioBuffer<float> block(1, 127);
        engine.process(block);
    }
    const bool pass = engine.getDetectedHz() == 0.0f;
    std::printf("detector: silence -> %.2f Hz %s\n", engine.getDetectedHz(), pass ? "PASS" : "FAIL");
    return pass;
}

bool runDecimatorAliasRejection(double sampleRate, int blockSize, double highFrequency)
{
    humtune::AutoTuneEngine engine;
    engine.prepare(sampleRate, blockSize);
    engine.setVoiceRange(2); // detector search includes 220 Hz and its octave region
    engine.setEnabled(true);

    const int total = static_cast<int>(sampleRate); // one second of input
    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sampleRate;
            // The second tone is deliberately strong. Before FIR anti-alias filtering,
            // at 44.1 kHz it aliases near 725 Hz in the detector's reduced stream and
            // can pull a 220 Hz estimate away from the true fundamental.
            block.getWritePointer(0)[i] = 0.30f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * t))
                                         + 0.60f * static_cast<float>(std::sin(2.0 * kPi * highFrequency * t));
        }
        engine.process(block);
        offset += n;
    }

    const float detected = engine.getDetectedHz();
    const bool pass = detected > 0.0f && std::abs(detected - 220.0f) < 3.0f;
    std::printf("detector anti-alias: %.1f kHz, block %d, 220 Hz + strong %.0f Hz -> %.2f Hz conf %.3f %s\n",
                sampleRate / 1000.0, blockSize, highFrequency, detected,
                engine.getConfidence(), pass ? "PASS" : "FAIL");
    return pass;
}

struct EngineLagMetrics { int configured = 0, measured = 0; double correlation = 0.0; };

EngineLagMetrics measureEngineLag(double sampleRate, int blockSize)
{
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(0); // cover the whole test sweep without a range-floor edge.
    engine.setLowLatencyMode(true);
    engine.setAmount(0.0f); // unity pitch ratio isolates the processing delay.
    engine.setEnabled(true);
    engine.prepare(sampleRate, blockSize);

    const int total = static_cast<int>(sampleRate);
    std::vector<float> input(static_cast<size_t>(total));
    std::vector<float> output(static_cast<size_t>(total));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        const double hz = 180.0 + 80.0 * t;
        input[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(phase));
        phase += 2.0 * kPi * hz / sampleRate;
    }

    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        std::copy(input.begin() + offset, input.begin() + offset + n, block.getWritePointer(0));
        engine.process(block);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + n, output.begin() + offset);
        offset += n;
    }

    EngineLagMetrics result;
    result.configured = engine.getLatencySamples();
    const int firstLag = std::max(0, result.configured - 32);
    const int lastLag = result.configured + 32;
    const int begin = static_cast<int>(sampleRate * 0.35);
    const int end = static_cast<int>(sampleRate * 0.80);
    double best = -1.0;
    for (int lag = firstLag; lag <= lastLag; ++lag)
    {
        double xy = 0.0, xx = 0.0, yy = 0.0;
        // The probe is a low-frequency tone/sweep. Sampling every fourth point keeps
        // the full recording window and every candidate integer lag while making
        // sanitizer builds much cheaper; 12/24 kHz effective sample rates remain
        // far above the 180-260 Hz test content.
        for (int i = begin; i < end; i += 4)
        {
            const double a = output[static_cast<size_t>(i)];
            const double b = input[static_cast<size_t>(i - lag)];
            xy += a * b;
            xx += b * b;
            yy += a * a;
        }
        const double corr = xy / std::sqrt(std::max(1.0e-20, xx * yy));
        if (corr > best)
        {
            best = corr;
            result.measured = lag;
            result.correlation = corr;
        }
    }
    return result;
}

bool runEngineLagRegressions()
{
    bool pass = true;
    for (const int blockSize : { 32, 64, 127, 128, 192, 256, 511, 1024 })
    {
        const auto r = measureEngineLag(48000.0, blockSize);
        const bool thisPass = std::abs(r.measured - r.configured) <= 2
                           && r.measured < static_cast<int>(48000.0 * 0.002)
                           && r.correlation > 0.90;
        std::printf("engine lag @48k block %d: configured %d, measured %d, corr %.4f %s\n",
                    blockSize, r.configured, r.measured, r.correlation, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    for (const int blockSize : { 32, 128, 256, 512, 1024 })
    {
        const auto r = measureEngineLag(44100.0, blockSize);
        const bool thisPass = std::abs(r.measured - r.configured) <= 2
                           && r.measured < static_cast<int>(44100.0 * 0.002)
                           && r.correlation > 0.90;
        std::printf("engine lag @44.1k block %d: configured %d, measured %d, corr %.4f %s\n",
                    blockSize, r.configured, r.measured, r.correlation, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    for (const double sr : { 88200.0, 96000.0 })
    {
        for (const int blockSize : { 32, 64, 128, 256, 512, 1024 })
        {
            const auto r = measureEngineLag(sr, blockSize);
            const bool thisPass = std::abs(r.measured - r.configured) <= 2
                               && r.measured < static_cast<int>(sr * 0.002)
                               && r.correlation > 0.90;
            std::printf("engine lag @%.1fk block %d: configured %d, measured %d, corr %.4f %s\n",
                        sr / 1000.0, blockSize, r.configured, r.measured, r.correlation,
                        thisPass ? "PASS" : "FAIL");
            pass = thisPass && pass;
        }
    }
    return pass;
}

bool runDelayedBypassRegression()
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    constexpr int total = 48000;
    constexpr int switchAt = blockSize * 50;

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(2);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    std::vector<float> input(static_cast<size_t>(total));
    std::vector<float> output(static_cast<size_t>(total));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double t = static_cast<double>(i) / sr;
        const double hz = 180.0 + 80.0 * t;
        input[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(phase));
        phase += 2.0 * kPi * hz / sr;
    }

    for (int offset = 0; offset < total;)
    {
        if (offset == switchAt)
            engine.setEnabled(false);
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        std::copy(input.begin() + offset, input.begin() + offset + n, block.getWritePointer(0));
        engine.process(block);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + n, output.begin() + offset);
        offset += n;
    }

    const int latency = engine.getLatencySamples();
    const int begin = static_cast<int>(sr * 0.35);
    const int end = static_cast<int>(sr * 0.80);
    int bestLag = -1;
    double best = -1.0;
    for (int lag = std::max(0, latency - 8); lag <= latency + 8; ++lag)
    {
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (int i = begin; i < end; ++i)
        {
            const double a = output[static_cast<size_t>(i)];
            const double b = input[static_cast<size_t>(i - lag)];
            xy += a * b;
            xx += b * b;
            yy += a * a;
        }
        const double corr = xy / std::sqrt(std::max(1.0e-20, xx * yy));
        if (corr > best)
        {
            best = corr;
            bestLag = lag;
        }
    }
    const bool pass = bestLag == latency && best > 0.99999;
    std::printf("delayed bypass: configured %d, measured %d, corr %.6f %s\n",
                latency, bestLag, best, pass ? "PASS" : "FAIL");
    return pass;
}



bool runSampleRateReprepareRegression()
{
    constexpr int blockSize = 127;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(0);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);

    auto feed = [&](double sampleRate, int blockCount, int startSample)
    {
        for (int blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                const int sample = startSample + blockIndex * blockSize + i;
                block.getWritePointer(0)[i] = 0.3f * static_cast<float>(
                    std::sin(2.0 * kPi * 220.0 * static_cast<double>(sample) / sampleRate));
            }
            engine.process(block);
        }
    };

    engine.prepare(44100.0, blockSize);
    feed(44100.0, 24, 0);
    const float before = engine.getDetectedHz();
    engine.prepare(48000.0, blockSize);
    feed(48000.0, 40, 0);
    const float after = engine.getDetectedHz();
    const int expectedLatency = static_cast<int>(std::ceil(48000.0 * 0.0019));
    const bool pass = std::abs(before - 220.0f) < 4.0f
                   && std::abs(after - 220.0f) < 4.0f
                   && engine.getLatencySamples() == expectedLatency;
    std::printf("sample-rate re-prepare: 44.1k detected %.2f Hz, 48k detected %.2f Hz, latency %d/%d %s\n",
                before, after, engine.getLatencySamples(), expectedLatency, pass ? "PASS" : "FAIL");
    return pass;
}

bool runVoiceRangeHistoryResetRegression()
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(0);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    int64_t globalSample = 0;
    auto feedTone = [&](double hz, int blockCount)
    {
        for (int blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        {
            juce::AudioBuffer<float> block(1, blockSize);
            for (int i = 0; i < blockSize; ++i, ++globalSample)
                block.getWritePointer(0)[i] = 0.3f * static_cast<float>(
                    std::sin(2.0 * kPi * hz * static_cast<double>(globalSample) / sr));
            engine.process(block);
        }
    };

    feedTone(220.0, 40);
    const float beforeRangeChange = engine.getDetectedHz();
    engine.setVoiceRange(4); // raises detector floor from 70 to 180 Hz
    feedTone(329.627, 4);    // 508 samples: less than the fresh 536-sample YIN window
    const float duringWarmup = engine.getDetectedHz();
    feedTone(329.627, 8);
    const float afterWarmup = engine.getDetectedHz();

    const bool pass = std::abs(beforeRangeChange - 220.0f) < 4.0f
                   && duringWarmup == 0.0f
                   && std::abs(afterWarmup - 329.627f) < 4.0f;
    std::printf("range-change history: before %.2f Hz, fresh-window warmup %.2f Hz, after %.2f Hz %s\n",
                beforeRangeChange, duringWarmup, afterWarmup, pass ? "PASS" : "FAIL");
    return pass;
}

bool runActiveCorrectionRegression(double inputHz, double targetHz, int expectedTargetMidi, int blockSize)
{
    // Exercise active correction through the complete engine, with a spectral projection
    // rather than zero crossings (grain textures can add extra crossings).
    // This validates the actual correction path, not only detection or unity-ratio delay.
    constexpr double sr = 48000.0;
    constexpr int total = 48000;
    constexpr int measureBegin = 18000;
    constexpr int measureEnd = 42000;

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(0);
    engine.setRootNote(0);       // C
    engine.setScaleType(0);      // major
    engine.setAmount(1.0f);
    engine.setRetuneSpeed(0.0f); // fastest response
    engine.setHumanize(0.0f);
    engine.setNoteStabilizer(false);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    std::vector<float> output(static_cast<size_t>(total));
    int offset = 0;
    while (offset < total)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            block.getWritePointer(0)[i] = 0.3f * static_cast<float>(std::sin(2.0 * kPi * inputHz * t));
        }
        engine.process(block);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + n, output.begin() + offset);
        offset += n;
    }

    // Project the rendered output onto the target and input-frequency sinusoids. The
    // Hann-grain resynthesizer can introduce small waveform texture that creates extra
    // zero crossings, so a raw crossing count is not a reliable fundamental estimator.
    auto amplitudeAt = [&](double hz)
    {
        double re = 0.0, im = 0.0;
        for (int i = measureBegin; i < measureEnd; ++i)
        {
            const double phase = 2.0 * kPi * hz * static_cast<double>(i) / sr;
            const double x = output[static_cast<size_t>(i)];
            re += x * std::cos(phase);
            im += x * std::sin(phase);
        }
        return 2.0 * std::hypot(re, im) / static_cast<double>(measureEnd - measureBegin);
    };
    const double targetAmplitude = amplitudeAt(targetHz);
    const double inputAmplitude = amplitudeAt(inputHz);
    const int targetMidi = engine.getTargetMidiNote();
    const float detectedHz = engine.getDetectedHz();
    const bool pass = targetAmplitude > 0.08
                   && targetAmplitude > inputAmplitude * 3.0
                   && targetMidi == expectedTargetMidi
                   && std::abs(detectedHz - static_cast<float>(inputHz)) < 4.0f
                   && std::abs(engine.getTargetHz() - targetHz) < 0.1f;
    std::printf("active correction: block %d, input %.2f Hz -> target MIDI %d / %.2f Hz (expected MIDI %d / %.2f Hz), spectral amplitude target %.4f vs source %.4f, detected %.2f Hz, correction %.2f cents %s\n",
                blockSize, inputHz, targetMidi, engine.getTargetHz(), expectedTargetMidi, targetHz,
                targetAmplitude, inputAmplitude, detectedHz, engine.getCorrectionCents(), pass ? "PASS" : "FAIL");
    return pass;
}

bool runSubBassActiveCorrectionRegression(int blockSize, double sr = 48000.0)
{
    const int total = static_cast<int>(sr);
    constexpr double inputHz = 41.2035; // E1
    constexpr double targetHz = 43.6535; // F1: nearest allowed note in C# major
    constexpr int targetMidiExpected = 29;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(5);       // Sub Bass range, 40 Hz floor
    engine.setRootNote(1);         // C# major: E natural is corrected to F
    engine.setScaleType(0);
    engine.setAmount(1.0f);
    engine.setRetuneSpeed(0.0f);
    engine.setHumanize(0.0f);
    engine.setNoteStabilizer(false);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    std::vector<float> output(static_cast<size_t>(total));
    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
            block.getWritePointer(0)[i] = 0.3f * static_cast<float>(std::sin(
                2.0 * kPi * inputHz * static_cast<double>(offset + i) / sr));
        engine.process(block);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + n, output.begin() + offset);
        offset += n;
    }

    auto amplitudeAt = [&](double hz)
    {
        double re = 0.0, im = 0.0;
        const int begin = static_cast<int>(sr * 0.375);
        const int end = static_cast<int>(sr * 0.875);
        for (int i = begin; i < end; ++i)
        {
            const double phase = 2.0 * kPi * hz * static_cast<double>(i) / sr;
            const double x = output[static_cast<size_t>(i)];
            re += x * std::cos(phase);
            im += x * std::sin(phase);
        }
        return 2.0 * std::hypot(re, im) / static_cast<double>(end - begin);
    };
    const double targetAmp = amplitudeAt(targetHz);
    const double sourceAmp = amplitudeAt(inputHz);
    const int targetMidi = engine.getTargetMidiNote();
    const float detectedHz = engine.getDetectedHz();
    const bool pass = targetAmp > 0.04 && targetAmp > sourceAmp * 2.0
                   && targetMidi == targetMidiExpected
                   && std::abs(detectedHz - static_cast<float>(inputHz)) < 1.5f
                   && std::abs(engine.getTargetHz() - static_cast<float>(targetHz)) < 0.15f
                   && engine.getLatencySamples() == static_cast<int>(std::ceil(sr * 0.0019));
    std::printf("sub-bass active correction %.1fkHz block %d: E1 %.2f Hz -> F1 %.2f Hz; target/source %.4f/%.4f; detector %.2f; MIDI %d; lag %d %s\n",
                sr / 1000.0, blockSize, inputHz, targetHz, targetAmp, sourceAmp, detectedHz,
                targetMidi, engine.getLatencySamples(), pass ? "PASS" : "FAIL");
    return pass;
}

bool runSubBassLatencyModeRegression()
{
    constexpr double sr = 48000.0;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(5);
    engine.prepare(sr, 127);
    const int standardExpected = static_cast<int>(std::ceil(1.3 * std::ceil(sr / 40.0))) + 8;
    const int standardMeasured = engine.getLatencySamples();
    engine.setLowLatencyMode(true);
    const int lowMeasured = engine.getLatencySamples();
    engine.setLowLatencyMode(false);
    const int restoredMeasured = engine.getLatencySamples();
    const bool pass = standardMeasured == standardExpected && lowMeasured == 92
                   && restoredMeasured == standardExpected;
    std::printf("Sub Bass latency toggle: standard %d (expected %d), experimental %d (expected 92), restored %d %s\n",
                standardMeasured, standardExpected, lowMeasured, restoredMeasured, pass ? "PASS" : "FAIL");
    return pass;
}

bool runLowLatencyEngineIntegration()
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(2); // 100 Hz minimum; the experimental path uses fixed delay.
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    const int expectedLowLatency = static_cast<int>(std::ceil(sr * 0.0019));
    bool pass = engine.getLowLatencyMode()
             && engine.getLatencySamples() == expectedLowLatency;

    int offset = 0;
    constexpr int total = 24000;
    while (offset < total)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(2, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            const float x = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * t));
            block.getWritePointer(0)[i] = x;
            block.getWritePointer(1)[i] = x;
        }
        engine.process(block);
        offset += n;
    }
    const float detected = engine.getDetectedHz();
    pass = pass && detected > 0.0f && std::abs(detected - 220.0f) < 4.0f;

    // Verify runtime toggling reconfigures both engine timing and mode state.
    const int measuredLowLatency = engine.getLatencySamples();
    engine.setLowLatencyMode(false);
    const int expectedStandardLatency = static_cast<int>(std::ceil(1.3 * std::ceil(sr / 100.0))) + 8;
    const int measuredStandardLatency = engine.getLatencySamples();
    pass = pass && !engine.getLowLatencyMode()
                && measuredStandardLatency == expectedStandardLatency;
    engine.setLowLatencyMode(true);
    const int reenabledLatency = engine.getLatencySamples();
    pass = pass && engine.getLowLatencyMode()
                && reenabledLatency == expectedLowLatency;

    std::printf("engine integration: low latency %d samples (expected %d), standard %d samples (expected %d), re-enabled %d samples, detected %.2f Hz %s\n",
                measuredLowLatency, expectedLowLatency, measuredStandardLatency,
                expectedStandardLatency, reenabledLatency, detected, pass ? "PASS" : "FAIL");
    return pass;
}


std::vector<float> renderChangingVoiceForBlockSize(int blockSize, float formantShiftSemitones = 0.0f, float flexTune = 0.0f)
{
    constexpr double sr = 48000.0;
    constexpr int total = static_cast<int>(sr * 2.5);
    std::vector<float> input(static_cast<size_t>(total));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double t = static_cast<double>(i) / sr;
        const double glide = 200.0 + 35.0 * std::sin(2.0 * kPi * 0.37 * t);
        const double phraseJump = (t > 0.85 && t < 1.25) ? 25.0 : ((t >= 1.85) ? -30.0 : 0.0);
        phase += 2.0 * kPi * (glide + phraseJump) / sr;
        // A deterministic harmonic-rich voice-like signal rather than a pure sine.
        input[static_cast<size_t>(i)] = static_cast<float>(
            0.20 * std::sin(phase)
          + 0.09 * std::sin(2.0 * phase + 0.3)
          + 0.04 * std::sin(3.0 * phase + 0.8)
          + 0.02 * std::sin(4.0 * phase));
    }

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(3); // 130 Hz low floor, suitable for this synthetic phrase.
    engine.setRootNote(0);
    engine.setScaleType(0);
    engine.setAmount(1.0f);
    engine.setRetuneSpeed(0.0f);
    engine.setHumanize(0.0f);
    engine.setNoteStabilizer(false);
    engine.setFormantShiftSemitones(formantShiftSemitones);
    engine.setFlexTune(flexTune);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    std::vector<float> output(static_cast<size_t>(total));
    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        std::copy(input.begin() + offset, input.begin() + offset + n, block.getWritePointer(0));
        engine.process(block);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + n, output.begin() + offset);
        offset += n;
    }
    return output;
}

bool runUnvoicedCorrectionReleaseRegression()
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 192; // exactly one detector hop at 48 kHz in this build
    constexpr double inputHz = 228.0;
    constexpr int toneSamples = 48000; // one second to establish a stable correction
    constexpr int silenceSamples = 24000;

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(2); // 100 Hz floor
    engine.setRootNote(0);
    engine.setScaleType(0);  // C major: 228 Hz is corrected toward A3 / 220 Hz
    engine.setAmount(1.0f);
    engine.setRetuneSpeed(0.0f);
    engine.setHumanize(0.0f);
    engine.setNoteStabilizer(true);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    int inputOffset = 0;
    while (inputOffset < toneSamples)
    {
        const int n = std::min(blockSize, toneSamples - inputOffset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
            block.getWritePointer(0)[i] = 0.35f * std::sin(
                2.0 * juce::MathConstants<double>::pi * inputHz * (inputOffset + i) / sr);
        engine.process(block);
        inputOffset += n;
    }

    const float ratioBeforeGap = engine.getAppliedPitchRatio();
    bool sawUnvoiced = false;
    bool firstReleaseWasSmooth = false;
    bool releaseWasMonotonic = true;
    bool reachedUnity = false;
    int unvoicedHops = 0;
    float previousRatio = ratioBeforeGap;
    float firstUnvoicedRatio = ratioBeforeGap;
    float lateSilencePeak = 0.0f;

    int silenceOffset = 0;
    while (silenceOffset < silenceSamples)
    {
        const int n = std::min(blockSize, silenceSamples - silenceOffset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
            block.getWritePointer(0)[i] = 0.0f;
        engine.process(block);

        const float ratio = engine.getAppliedPitchRatio();
        if (engine.getDetectedHz() <= 0.0f)
        {
            ++unvoicedHops;
            if (!sawUnvoiced)
            {
                sawUnvoiced = true;
                firstUnvoicedRatio = ratio;
                // A short dropout must move toward unity but not hard-switch in one hop.
                firstReleaseWasSmooth = std::abs(1.0f - ratio) + 1.0e-5f
                                           < std::abs(1.0f - ratioBeforeGap)
                                      && std::abs(1.0f - ratio) > 1.0e-4f;
            }
            else if (std::abs(1.0f - ratio) > std::abs(1.0f - previousRatio) + 1.0e-5f)
                releaseWasMonotonic = false;
            previousRatio = ratio;
        }

        // The resynthesis tail should fully drain in silence; after 60 ms there must
        // be no repeated predictor-cycle leakage at the output.
        if (silenceOffset + n > static_cast<int>(0.060 * sr))
            for (int i = 0; i < n; ++i)
                lateSilencePeak = std::max(lateSilencePeak, std::abs(block.getReadPointer(0)[i]));

        if (sawUnvoiced && unvoicedHops >= 6 && std::abs(1.0f - ratio) < 1.0e-6f)
            reachedUnity = true;
        silenceOffset += n;
    }

    const bool activeCorrection = std::abs(1.0f - ratioBeforeGap) > 0.01f;
    const bool pass = activeCorrection && sawUnvoiced && firstReleaseWasSmooth
                   && releaseWasMonotonic && reachedUnity && lateSilencePeak < 1.0e-6f;
    std::printf("unvoiced release: active %.6f -> first unvoiced %.6f, hops %d, smooth %s, monotonic %s, unity %s, late-silence peak %.8f %s\n",
                ratioBeforeGap, firstUnvoicedRatio, unvoicedHops,
                firstReleaseWasSmooth ? "yes" : "no",
                releaseWasMonotonic ? "yes" : "no",
                reachedUnity ? "yes" : "no", lateSilencePeak,
                pass ? "PASS" : "FAIL");
    return pass;
}

struct StabilizerVibratoResult
{
    int targetTransitions = 0;
    int validFrames = 0;
    int framesOutsideExpectedTarget = 0;
};

StabilizerVibratoResult renderStabilizerVibrato(bool stabilizerEnabled, double vibratoCents)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 192; // one detector hop at 48 kHz
    constexpr double baseHz = 246.94165; // B3, adjacent to C4 in C major
    constexpr double vibratoHz = 5.0;
    constexpr int totalSamples = static_cast<int>(sr * 2.4);
    constexpr int warmupSamples = static_cast<int>(sr * 0.20);

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(2); // 100 Hz floor
    engine.setRootNote(0);
    engine.setScaleType(0); // C major
    engine.setAmount(1.0f);
    engine.setRetuneSpeed(0.0f);
    engine.setHumanize(0.0f);
    engine.setNoteStabilizer(stabilizerEnabled);
    engine.setSustain(60.0f);
    engine.setLowLatencyMode(false); // validate the user's quality-first mode
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    StabilizerVibratoResult result;
    int previousTarget = -1;
    double phase = 0.0;
    int processed = 0;
    while (processed < totalSamples)
    {
        const int n = std::min(blockSize, totalSamples - processed);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(processed + i) / sr;
            const double cents = vibratoCents * std::sin(2.0 * kPi * vibratoHz * t);
            const double hz = baseHz * std::pow(2.0, cents / 1200.0);
            block.getWritePointer(0)[i] = 0.30f * static_cast<float>(std::sin(phase));
            phase += 2.0 * kPi * hz / sr;
        }
        engine.process(block);

        if (processed >= warmupSamples)
        {
            const int target = engine.getTargetMidiNote();
            if (target >= 0)
            {
                ++result.validFrames;
                if (target != 59) // B3 is intended; C4 changes are vibrato-induced target chatter
                    ++result.framesOutsideExpectedTarget;
                if (previousTarget >= 0 && target != previousTarget)
                    ++result.targetTransitions;
                previousTarget = target;
            }
        }
        processed += n;
    }
    return result;
}

bool runNoteStabilizerVibratoRegression()
{
    // A 5 Hz, +/-55-cent vibrato around B3 deliberately crosses the B3/C4
    // quantization midpoint while staying inside the 60-cent note-lock window.
    // A functioning Note Stabilizer should keep the target on B3, while disabling
    // it should expose the target chatter. This is the exact behavior the UI tooltip
    // promises and directly protects the user's preferred quality-first mode.
    const auto without = renderStabilizerVibrato(false, 55.0);
    const auto with = renderStabilizerVibrato(true, 55.0);
    const bool pass = without.validFrames > 100
                   && with.validFrames > 100
                   && without.targetTransitions >= 4
                   && with.targetTransitions == 0
                   && with.framesOutsideExpectedTarget == 0;
    std::printf("Note Stabilizer vibrato: off %d target transitions / %d non-B frames; on %d transitions / %d non-B frames (%d valid) %s\n",
                without.targetTransitions, without.framesOutsideExpectedTarget,
                with.targetTransitions, with.framesOutsideExpectedTarget, with.validFrames,
                pass ? "PASS" : "FAIL");
    return pass;
}

bool runHostBlockWaveformConsistencyRegression()
{
    const auto reference = renderChangingVoiceForBlockSize(64);
    bool pass = true;
    for (const int blockSize : { 127, 192, 256, 511, 1024 })
    {
        const auto candidate = renderChangingVoiceForBlockSize(blockSize);
        double diffEnergy = 0.0, referenceEnergy = 0.0;
        double maxAbsDiff = 0.0;
        // Skip the initial detector fill, then compare actual waveforms at identical
        // sample indices. This catches callback-boundary-dependent correction updates,
        // not just similar long-term pitch or RMS values.
        const int begin = 12000;
        for (int i = begin; i < static_cast<int>(reference.size()); ++i)
        {
            const double a = reference[static_cast<size_t>(i)];
            const double b = candidate[static_cast<size_t>(i)];
            const double d = a - b;
            diffEnergy += d * d;
            referenceEnergy += a * a;
            maxAbsDiff = std::max(maxAbsDiff, std::abs(d));
        }
        const double relativeRmsDiff = std::sqrt(diffEnergy / std::max(1.0e-20, referenceEnergy));
        const bool thisPass = relativeRmsDiff < 1.0e-5 && maxAbsDiff < 1.0e-5;
        std::printf("host-block waveform consistency: reference 64, candidate %d, relative RMS diff %.9f, max sample diff %.9f %s\n",
                    blockSize, relativeRmsDiff, maxAbsDiff, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    return pass;
}

bool runFormantShiftHostBlockConsistencyRegression()
{
    // The new formant control must follow a persistent sample timeline just like
    // the existing pitch control; callback sizes may split a 32-sample slice but
    // must not accelerate or reset its smoothing ramp.
    const auto reference = renderChangingVoiceForBlockSize(64, 2.5f);
    bool pass = true;
    for (const int blockSize : { 127, 192, 256, 511, 1024 })
    {
        const auto candidate = renderChangingVoiceForBlockSize(blockSize, 2.5f);
        double diffEnergy = 0.0, referenceEnergy = 0.0, maxAbsDiff = 0.0;
        for (int i = 12000; i < static_cast<int>(reference.size()); ++i)
        {
            const double a = reference[static_cast<size_t>(i)];
            const double b = candidate[static_cast<size_t>(i)];
            const double d = a - b;
            diffEnergy += d * d;
            referenceEnergy += a * a;
            maxAbsDiff = std::max(maxAbsDiff, std::abs(d));
        }
        const double relativeRmsDiff = std::sqrt(diffEnergy / std::max(1.0e-20, referenceEnergy));
        const bool thisPass = relativeRmsDiff < 1.0e-5 && maxAbsDiff < 1.0e-5;
        std::printf("formant-shift host-block consistency: reference 64, candidate %d, relative RMS diff %.9f, max sample diff %.9f %s\n",
                    blockSize, relativeRmsDiff, maxAbsDiff, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    return pass;
}
}


struct HumanizeAutomationMetrics
{
    double automationJump = 0.0;
    double steadyJump = 0.0;
};

HumanizeAutomationMetrics renderHumanizeAutomation(bool automate)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    constexpr int total = static_cast<int>(sr * 1.4);
    constexpr int switchAt = static_cast<int>(sr * 0.7);

    humtune::AutoTuneEngine engine;
    engine.prepare(sr, blockSize);
    engine.setVoiceRange(2);
    engine.setRetuneSpeed(0.0f);
    engine.setHumanize(0.0f);
    engine.setEnabled(true);

    std::vector<float> rendered(static_cast<size_t>(total), 0.0f);
    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        if (automate && offset >= switchAt)
            engine.setHumanize(1.0f);

        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            block.getWritePointer(0)[i] = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 228.0 * t));
        }
        engine.process(block);
        std::copy(block.getReadPointer(0), block.getReadPointer(0) + n, rendered.begin() + offset);
        offset += n;
    }

    HumanizeAutomationMetrics metrics;
    const int window = static_cast<int>(sr * 0.025);
    const int begin = std::max(1, switchAt - window);
    const int end = std::min(total, switchAt + window);
    for (int i = begin; i < end; ++i)
        metrics.automationJump = std::max(metrics.automationJump,
            std::abs(static_cast<double>(rendered[static_cast<size_t>(i)]
                                      - rendered[static_cast<size_t>(i - 1)])));

    const int steadyBegin = static_cast<int>(sr * 0.35);
    const int steadyEnd = static_cast<int>(sr * 0.55);
    for (int i = steadyBegin + 1; i < steadyEnd; ++i)
        metrics.steadyJump = std::max(metrics.steadyJump,
            std::abs(static_cast<double>(rendered[static_cast<size_t>(i)]
                                      - rendered[static_cast<size_t>(i - 1)])));
    return metrics;
}

bool runHumanizeAutomationRegression()
{
    const auto changed = renderHumanizeAutomation(true);
    const auto steady = renderHumanizeAutomation(false);
    const bool pass = changed.automationJump < std::max(0.12, steady.steadyJump * 2.5);
    std::printf("Humanize automation smoothing: max step %.5f vs steady %.5f %s\n",
                changed.automationJump, steady.steadyJump, pass ? "PASS" : "FAIL");
    return pass;
}



bool runFlexTuneHostBlockConsistencyRegression()
{
    const auto reference = renderChangingVoiceForBlockSize(64, 0.0f, 0.85f);
    bool pass = true;
    for (const int blockSize : { 127, 192, 256, 511, 1024 })
    {
        const auto candidate = renderChangingVoiceForBlockSize(blockSize, 0.0f, 0.85f);
        double diffEnergy = 0.0, referenceEnergy = 0.0, maxAbsDiff = 0.0;
        for (int i = 12000; i < static_cast<int>(reference.size()); ++i)
        {
            const double a = reference[static_cast<size_t>(i)];
            const double b = candidate[static_cast<size_t>(i)];
            const double d = a - b;
            diffEnergy += d * d;
            referenceEnergy += a * a;
            maxAbsDiff = std::max(maxAbsDiff, std::abs(d));
        }
        const double relativeRmsDiff = std::sqrt(diffEnergy / std::max(1.0e-20, referenceEnergy));
        const bool thisPass = relativeRmsDiff < 1.0e-5 && maxAbsDiff < 1.0e-5;
        std::printf("Flex-Tune host-block consistency: reference 64, candidate %d, relative RMS diff %.9f, max sample diff %.9f %s\n",
                    blockSize, relativeRmsDiff, maxAbsDiff, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    return pass;
}

bool runMicrotonalTargetRegression()
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    constexpr int total = static_cast<int>(sr * 0.8);

    humtune::AutoTuneEngine engine;
    engine.prepare(sr, blockSize);
    engine.setVoiceRange(2);
    engine.setRootNote(0);
    engine.setScaleType(12); // Maqam Rast includes the E-half-flat target at +3.5 semitones.
    engine.clearCustomScale();
    engine.setHumanize(0.0f);
    engine.setRetuneSpeed(0.0f);
    engine.setEnabled(true);

    // MIDI 63.1 is between Eb and the Rast's Eb-half-sharp / E-half-flat degree.
    const double inputHz = 440.0 * std::pow(2.0, (63.1 - 69.0) / 12.0);
    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            block.getWritePointer(0)[i] = 0.25f * static_cast<float>(std::sin(2.0 * kPi * inputHz * t));
        }
        engine.process(block);
        offset += n;
    }

    const float target = engine.getTargetMidiFloat();
    const bool pass = target >= 0.0f && std::abs(target - 63.5f) < 0.12f;
    std::printf("Microtonal engine target: expected MIDI 63.5, got %.3f %s\n",
                target, pass ? "PASS" : "FAIL");
    return pass;
}


bool runFlexTuneCurveRegression()
{
    using humtune::AutoTuneEngine;
    const float neutralLarge = AutoTuneEngine::flexCorrectionWeight(180.0f, 0.0f);
    const float flexNear = AutoTuneEngine::flexCorrectionWeight(3.0f, 1.0f);
    const float flexMid = AutoTuneEngine::flexCorrectionWeight(12.0f, 1.0f);
    const float flexFar = AutoTuneEngine::flexCorrectionWeight(40.0f, 1.0f);
    const float mediumNear = AutoTuneEngine::flexCorrectionWeight(20.0f, 0.5f);
    const float mediumFar = AutoTuneEngine::flexCorrectionWeight(60.0f, 0.5f);

    const bool pass = neutralLarge == 1.0f
                   && flexNear > 0.99f
                   && flexMid > 0.0f && flexMid < 1.0f
                   && flexFar < 0.001f
                   && mediumNear > mediumFar
                   && mediumFar > 0.45f && mediumFar < 0.55f;
    std::printf("Flex-Tune curve: off %.2f, max-flex near %.2f mid %.2f far %.2f, medium far %.2f %s\n",
                neutralLarge, flexNear, flexMid, flexFar, mediumFar, pass ? "PASS" : "FAIL");
    return pass;
}

float renderFlexToneRatio(float flex, double inputHz)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    constexpr int total = static_cast<int>(sr * 0.8);
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(2);
    engine.setRootNote(0);
    engine.setScaleType(0); // C major; A3 is the nearest allowed target to 228 Hz.
    engine.setAmount(1.0f);
    engine.setHumanize(0.0f);
    engine.setFlexTune(flex);
    engine.setRetuneSpeed(0.0f);
    engine.setEnabled(true);
    engine.prepare(sr, blockSize);

    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        juce::AudioBuffer<float> block(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            block.getWritePointer(0)[i] = 0.25f * static_cast<float>(std::sin(2.0 * kPi * inputHz * t));
        }
        engine.process(block);
        offset += n;
    }
    return engine.getAppliedPitchRatio();
}

bool runFlexTuneEngineRegression()
{
    const float normal = renderFlexToneRatio(0.0f, 228.0);
    const float flexed = renderFlexToneRatio(1.0f, 228.0);
    const float nearNormal = renderFlexToneRatio(0.0f, 221.0);
    const float nearFlex = renderFlexToneRatio(1.0f, 221.0);

    const bool pass = normal < 0.99f
                   && std::abs(flexed - 1.0f) < 0.003f
                   && std::abs(nearNormal - 1.0f) > std::abs(nearFlex - 1.0f) * 0.85f;
    std::printf("Flex-Tune engine: 228 Hz ratio normal %.5f, flex %.5f; near-note 221 Hz normal %.5f, flex %.5f %s\n",
                normal, flexed, nearNormal, nearFlex, pass ? "PASS" : "FAIL");
    return pass;
}

struct PitchMotionMetrics
{
    double rawMae = 0.0;
    double predictedMae = 0.0;
    double rawP95 = 0.0;
    double predictedP95 = 0.0;
    double maxPredictionOffsetCents = 0.0;
    int frames = 0;
};

PitchMotionMetrics measurePitchMotionCompensation (int scenario)
{
    constexpr double sr = 48000.0;
    int hop = 192; // detector's 4 ms hop at 48 kHz; re-read below if Low Latency runs a faster hop
    constexpr double kCentsPerOctave = 1200.0;
    const int range = scenario == 2 ? 3 : 0;
    const double floorHz = humtune::AutoTuneEngine::kRangeMinHz[range];

    humtune::PitchDetector referenceDetector;
    referenceDetector.prepare(sr, hop);
    referenceDetector.setMinFrequency(static_cast<float>(floorHz));
    // The engine's detector reports estimates without the median once a Low Latency track is
    // established; the comparison detector must do the same to stay "like the engine's detector".
    referenceDetector.setFastResponse(true);
    hop = referenceDetector.getHopSize();

    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(range);
    engine.setLowLatencyMode(true);
    engine.setAmount(0.0f); // inspect tracking without changing the measured tone
    engine.setNoteStabilizer(false);
    engine.setEnabled(true);
    engine.prepare(sr, hop);

    std::vector<double> rawErrors;
    std::vector<double> predictedErrors;
    PitchMotionMetrics result;
    double phase = 0.0;
    constexpr int total = static_cast<int>(sr * 2.0);

    for (int offset = 0; offset < total; offset += hop)
    {
        juce::AudioBuffer<float> block(1, hop);
        float* input = block.getWritePointer(0);
        for (int i = 0; i < hop; ++i)
        {
            const double t = static_cast<double>(offset + i) / sr;
            double hz = 110.0;
            switch (scenario)
            {
                case 0: hz = 110.0 + 8.0 * std::sin(2.0 * kPi * 5.0 * t); break;
                case 1: hz = 80.0 + 130.0 * t / 2.0; break; // legato rise from 80 to 210 Hz
                case 2: hz = 220.0 + 12.0 * std::sin(2.0 * kPi * 5.0 * t); break;
                case 3: hz = 196.0 + 4.0 * std::sin(2.0 * kPi * 6.0 * t); break;
                default:
                    hz = t < 0.8 ? 110.0 : (t < 1.3 ? 220.0 : 164.81);
                    break;
            }

            phase += 2.0 * kPi * hz / sr;
            double voiced = 0.25 * std::sin(phase);
            for (int harmonic = 2; harmonic <= 12; ++harmonic)
                voiced += (0.5 / static_cast<double>(harmonic))
                        * std::sin(phase * harmonic);
            input[i] = static_cast<float>(0.1 * voiced);
        }

        // The comparison detector sees the unmodified input, like the engine's detector.
        referenceDetector.feedSamples(input, hop);
        const float rawHz = referenceDetector.detectPitch();
        engine.process(block);
        const float predictedHz = engine.getDetectedHz();
        const double endTime = static_cast<double>(offset + hop) / sr;

        if (endTime < 0.30 || rawHz <= 0.0f || predictedHz <= 0.0f)
            continue;

        double referenceHz = 110.0;
        switch (scenario)
        {
            case 0: referenceHz = 110.0 + 8.0 * std::sin(2.0 * kPi * 5.0 * endTime); break;
            case 1: referenceHz = 80.0 + 130.0 * endTime / 2.0; break;
            case 2: referenceHz = 220.0 + 12.0 * std::sin(2.0 * kPi * 5.0 * endTime); break;
            case 3: referenceHz = 196.0 + 4.0 * std::sin(2.0 * kPi * 6.0 * endTime); break;
            default:
                referenceHz = endTime < 0.8 ? 110.0 : (endTime < 1.3 ? 220.0 : 164.81);
                break;
        }

        const double rawCents = std::abs(kCentsPerOctave * std::log2(rawHz / referenceHz));
        const double predictedCents = std::abs(kCentsPerOctave * std::log2(predictedHz / referenceHz));
        const double forecastOffset = std::abs(kCentsPerOctave * std::log2(predictedHz / rawHz));
        result.maxPredictionOffsetCents = std::max(result.maxPredictionOffsetCents, forecastOffset);
        rawErrors.push_back(rawCents);
        predictedErrors.push_back(predictedCents);
    }

    auto summarize = [](std::vector<double>& values, double& mean, double& p95)
    {
        if (values.empty())
            return;
        double sum = 0.0;
        for (double v : values) sum += v;
        mean = sum / static_cast<double>(values.size());
        std::sort(values.begin(), values.end());
        p95 = values[std::min(values.size() - 1,
                             (values.size() - 1) * 95 / 100)];
    };
    summarize(rawErrors, result.rawMae, result.rawP95);
    summarize(predictedErrors, result.predictedMae, result.predictedP95);
    result.frames = static_cast<int>(rawErrors.size());
    return result;
}

bool runPitchMotionPredictionRegression()
{
    const char* labels[] = {"110 Hz vibrato", "legato glide", "220 Hz vibrato", "subtle vibrato", "note jumps"};
    bool pass = true;
    for (int scenario = 0; scenario < 5; ++scenario)
    {
        const auto metrics = measurePitchMotionCompensation(scenario);
        const bool improves = scenario == 4
            ? (metrics.predictedMae <= metrics.rawMae * 1.15 + 1.0)
            : (metrics.predictedMae < metrics.rawMae * 0.78
               && metrics.predictedP95 <= metrics.rawP95 + 1.0);
        const bool bounded = metrics.maxPredictionOffsetCents <= 85.05;
        const bool thisPass = metrics.frames > 100 && improves && bounded;
        std::printf("pitch motion %s: raw MAE %.1f cents / p95 %.1f, compensated %.1f / %.1f, max offset %.1f cents, %d frames %s\n",
                    labels[scenario], metrics.rawMae, metrics.rawP95,
                    metrics.predictedMae, metrics.predictedP95, metrics.maxPredictionOffsetCents, metrics.frames,
                    thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    return pass;
}

int main()
{
    const DetectorCase cases[] = {
        {41.2035, 5, 127, false}, // E1: new backward-compatible Sub Bass range
        {82.4069, 0, 127, false}, // E2: lowest register of the legacy Bass range
        {110.0,   0, 256, false},
        {220.0,   2, 256, true},  // L/R cancellation fallback
        {329.627, 3, 64,  false},
        {440.0,   4, 511, false}, // irregular/oversized host blocks
        {1000.0,  4, 127, false},
        {1450.0,  4, 128, false}  // near the detector's 1,500 Hz tracking ceiling
    };
    bool pass = true;
    for (const auto& c : cases)
        pass = runTone(c) && pass;
    pass = runSilence() && pass;
    pass = runDecimatorAliasRejection(44100.0, 127, 10300.0) && pass;
    pass = runDecimatorAliasRejection(48000.0, 127, 10300.0) && pass;
    pass = runDecimatorAliasRejection(96000.0, 127, 10300.0) && pass;
    pass = runSubBassLatencyModeRegression() && pass;
    for (const int subBassBlockSize : { 64, 127, 256, 1024 })
        pass = runSubBassActiveCorrectionRegression(subBassBlockSize) && pass;
    for (const double subBassRate : { 44100.0, 88200.0, 96000.0 })
        pass = runSubBassActiveCorrectionRegression(127, subBassRate) && pass;
    pass = runLowLatencyEngineIntegration() && pass;
    pass = runVoiceRangeHistoryResetRegression() && pass;
    pass = runSampleRateReprepareRegression() && pass;
    for (const int blockSize : { 64, 127, 256, 1024 })
    {
        pass = runActiveCorrectionRegression(228.0, 220.0, 57, blockSize) && pass; // tune down to A3
        pass = runActiveCorrectionRegression(240.0, 246.94165, 59, blockSize) && pass; // tune up to B3
    }
    pass = runDelayedBypassRegression() && pass;
    pass = runEngineLagRegressions() && pass;
    pass = runUnvoicedCorrectionReleaseRegression() && pass;
    pass = runNoteStabilizerVibratoRegression() && pass;
    pass = runHumanizeAutomationRegression() && pass;
    pass = runFlexTuneCurveRegression() && pass;
    pass = runFlexTuneEngineRegression() && pass;
    pass = runFlexTuneHostBlockConsistencyRegression() && pass;
    pass = runHostBlockWaveformConsistencyRegression() && pass;
    pass = runFormantShiftHostBlockConsistencyRegression() && pass;
    pass = runMicrotonalTargetRegression() && pass;
    pass = runPitchMotionPredictionRegression() && pass;
    return pass ? 0 : 1;
}
