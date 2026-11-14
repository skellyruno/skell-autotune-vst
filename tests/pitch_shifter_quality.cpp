#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Result { double frequency = 0.0, rms = 0.0; int latency = 0; };

Result render(double inputHz, double ratio, double sampleRate, int blockSize, bool formants, bool lowLatency = false, double minHz = 100.0, double experimentalBudgetMs = 3.5, double formantShiftSemitones = 0.0)
{
    humtune::PitchShifter shifter;
    shifter.prepare(sampleRate, blockSize);
    shifter.setMinFrequency(minHz);
    shifter.setLowLatencyExperiment(lowLatency);
    shifter.setLowLatencyBudgetMilliseconds(experimentalBudgetMs);
    shifter.setFormantPreserve(formants);
    shifter.setFormantShiftSemitones(static_cast<float>(formantShiftSemitones));
    Result result;
    result.latency = shifter.getLatencySamples();

    const int total = static_cast<int>(sampleRate * 1.5);
    std::vector<float> output(static_cast<size_t>(total));
    int offset = 0;
    while (offset < total)
    {
        const int n = std::min(blockSize, total - offset);
        std::vector<float> block(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i);
            block[static_cast<size_t>(i)] = 0.35f * static_cast<float>(std::sin(2.0 * kPi * inputHz * t / sampleRate));
        }
        shifter.process(block.data(), n, static_cast<float>(sampleRate / inputHz), static_cast<float>(ratio));
        std::copy(block.begin(), block.end(), output.begin() + offset);
        offset += n;
    }

    const int begin = static_cast<int>(sampleRate * 0.35);
    const int end = total - static_cast<int>(sampleRate * 0.1);
    double sumSq = 0.0;
    for (int i = begin; i < end; ++i)
    {
        const double x = output[static_cast<size_t>(i)];
        sumSq += x * x;
    }

    // Zero-crossing count over-reads the pitch of TD-PSOLA output because the grain
    // texture can add small local crossings. Estimate the dominant fundamental from a
    // narrow sinusoidal-projection sweep around the expected shifted frequency instead.
    // It evaluates the actual rendered spectrum, not the requested ratio.
    const double expectedHz = inputHz * ratio;
    const double lowHz = std::max(20.0, expectedHz * 0.78);
    const double highHz = expectedHz * 1.22;
    double bestMagnitude = -1.0;
    double bestFrequency = expectedHz;
    const int windowLength = end - begin;
    for (double hz = lowHz; hz <= highHz; hz += 0.25)
    {
        const double stepCos = std::cos(2.0 * kPi * hz / sampleRate);
        const double stepSin = std::sin(2.0 * kPi * hz / sampleRate);
        double phaseCos = 1.0, phaseSin = 0.0;
        double re = 0.0, im = 0.0;
        for (int i = begin; i < end; ++i)
        {
            const double x = output[static_cast<size_t>(i)];
            re += x * phaseCos;
            im += x * phaseSin;
            const double nextCos = phaseCos * stepCos - phaseSin * stepSin;
            phaseSin = phaseSin * stepCos + phaseCos * stepSin;
            phaseCos = nextCos;
        }
        const double magnitude = std::hypot(re, im) * (2.0 / static_cast<double>(windowLength));
        if (magnitude > bestMagnitude)
        {
            bestMagnitude = magnitude;
            bestFrequency = hz;
        }
    }
    result.frequency = bestFrequency;
    result.rms = std::sqrt(sumSq / static_cast<double>(windowLength));
    return result;
}
}

struct EnvelopePeaks { double f1 = 0.0, f2 = 0.0; };

EnvelopePeaks renderFormantEnvelope(double formantShiftSemitones)
{
    constexpr double sampleRate = 48000.0;
    constexpr double fundamental = 180.0;
    constexpr int blockSize = 127;
    constexpr int total = 72000; // 1.5 seconds
    humtune::PitchShifter shifter;
    shifter.prepare(sampleRate, blockSize);
    shifter.setMinFrequency(70.0);
    shifter.setFormantPreserve(true);
    shifter.setFormantShiftSemitones(static_cast<float>(formantShiftSemitones));

    std::vector<float> output(static_cast<size_t>(total));
    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        std::vector<float> block(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(offset + i) / sampleRate;
            double x = 0.0;
            for (int harmonic = 1; harmonic <= 22; ++harmonic)
            {
                const double hz = fundamental * harmonic;
                const double g1 = std::exp(-0.5 * std::pow((hz - 720.0) / 150.0, 2.0));
                const double g2 = std::exp(-0.5 * std::pow((hz - 1980.0) / 250.0, 2.0));
                const double amplitude = 0.015 + 0.50 * g1 + 0.35 * g2;
                x += amplitude * std::sin(2.0 * kPi * hz * t);
            }
            block[static_cast<size_t>(i)] = static_cast<float>(x * 0.12);
        }
        shifter.process(block.data(), n, static_cast<float>(sampleRate / fundamental), 1.0f);
        std::copy(block.begin(), block.end(), output.begin() + offset);
        offset += n;
    }

    EnvelopePeaks peaks;
    double bestF1 = -1.0, bestF2 = -1.0;
    const int begin = 24000, end = 67200; // stable middle section after warm-up
    const int count = end - begin;
    for (int harmonic = 2; harmonic <= 18; ++harmonic)
    {
        const double hz = fundamental * harmonic;
        double re = 0.0, im = 0.0;
        for (int i = begin; i < end; ++i)
        {
            const double phase = 2.0 * kPi * hz * static_cast<double>(i) / sampleRate;
            re += static_cast<double>(output[static_cast<size_t>(i)]) * std::cos(phase);
            im -= static_cast<double>(output[static_cast<size_t>(i)]) * std::sin(phase);
        }
        const double magnitude = 2.0 * std::hypot(re, im) / static_cast<double>(count);
        if (hz >= 360.0 && hz <= 1170.0 && magnitude > bestF1)
        {
            bestF1 = magnitude;
            peaks.f1 = hz;
        }
        if (hz >= 1260.0 && hz <= 3240.0 && magnitude > bestF2)
        {
            bestF2 = magnitude;
            peaks.f2 = hz;
        }
    }
    return peaks;
}

bool runIndependentFormantEnvelopeRegression()
{
    const auto down = renderFormantEnvelope(-4.0);
    const auto neutral = renderFormantEnvelope(0.0);
    const auto up = renderFormantEnvelope(4.0);
    const bool pass = down.f1 < neutral.f1 && neutral.f1 < up.f1
                   && down.f2 < neutral.f2 && neutral.f2 < up.f2;
    std::printf("independent formant envelope: F1 %.0f/%.0f/%.0f Hz, F2 %.0f/%.0f/%.0f Hz (down/neutral/up) -> %s\n",
                down.f1, neutral.f1, up.f1, down.f2, neutral.f2, up.f2, pass ? "PASS" : "FAIL");
    return pass;
}

struct LagMetrics { int configured = 0, measured = 0; double correlation = 0.0; };

LagMetrics measureFixedDelayLag(double sampleRate, int blockSize, double budgetMs)
{
    humtune::PitchShifter shifter;
    shifter.prepare(sampleRate, blockSize);
    shifter.setMinFrequency(70.0);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(budgetMs);
    shifter.setFormantPreserve(true);

    const int total = static_cast<int>(sampleRate);
    std::vector<float> input(static_cast<size_t>(total));
    std::vector<float> output(static_cast<size_t>(total));
    double phase = 0.0;
    constexpr double testToneHz = 220.0;
    for (int i = 0; i < total; ++i)
    {
        input[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(phase));
        phase += 2.0 * kPi * testToneHz / sampleRate;
    }

    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        std::vector<float> block(input.begin() + offset, input.begin() + offset + n);
        shifter.process(block.data(), n, static_cast<float>(sampleRate / testToneHz), 1.0f);
        std::copy(block.begin(), block.end(), output.begin() + offset);
        offset += n;
    }

    LagMetrics result;
    result.configured = shifter.getLatencySamples();
    const int firstLag = std::max(0, result.configured - 24);
    const int lastLag = result.configured + 24;
    const int begin = static_cast<int>(sampleRate * 0.30);
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

bool runFixedDelayLagRegressions()
{
    bool pass = true;
    for (const int blockSize : { 32, 64, 127, 128, 256, 511, 1024 })
    {
        const auto r = measureFixedDelayLag(48000.0, blockSize, 1.9);
        const bool thisPass = std::abs(r.measured - r.configured) <= 2
                           && r.measured < static_cast<int>(48000.0 * 0.002)
                           && r.correlation > 0.90;
        std::printf("lag @48k/1.90ms block %d: configured %d, measured %d, corr %.4f %s\n",
                    blockSize, r.configured, r.measured, r.correlation, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    for (const int blockSize : { 32, 128, 256, 511, 1024 })
    {
        const auto r = measureFixedDelayLag(44100.0, blockSize, 1.9);
        const bool thisPass = std::abs(r.measured - r.configured) <= 2
                           && r.measured < static_cast<int>(44100.0 * 0.002)
                           && r.correlation > 0.90;
        std::printf("lag @44.1k/1.90ms block %d: configured %d, measured %d, corr %.4f %s\n",
                    blockSize, r.configured, r.measured, r.correlation, thisPass ? "PASS" : "FAIL");
        pass = thisPass && pass;
    }
    for (const double sr : { 88200.0, 96000.0 })
    {
        for (const int blockSize : { 32, 64, 128, 256, 512, 1024 })
        {
            const auto r = measureFixedDelayLag(sr, blockSize, 1.9);
            // At 88.2/96 kHz, 220 Hz phase correlation quantizes the effective
            // phase delay a few samples away from the nominal grain budget. The
            // separate under-2-ms guard is the main requirement here.
            const bool thisPass = std::abs(r.measured - r.configured) <= 4
                               && r.measured < static_cast<int>(sr * 0.002)
                               && r.correlation > 0.90;
            std::printf("lag @%.1fk/1.90ms block %d: configured %d, measured %d, corr %.4f %s\n",
                        sr / 1000.0, blockSize, r.configured, r.measured, r.correlation,
                        thisPass ? "PASS" : "FAIL");
            pass = thisPass && pass;
        }
    }
    return pass;
}

struct TransitionMetrics { double aroundJump = 0.0, steady = 0.0; };

TransitionMetrics renderPitchJump(bool fallingPitch, double budgetMs = 3.5,
                                  double lowHz = 220.0, double highHz = 330.0)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 128;
    constexpr int total = 48000;
    constexpr int jumpAt = total / 2;
    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(70.0);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(budgetMs);
    shifter.setFormantPreserve(true);

    std::vector<float> output(static_cast<size_t>(total));
    int offset = 0;
    double phase = 0.0;
    while (offset < total)
    {
        const int n = std::min(blockSize, total - offset);
        const double firstHz = (offset < jumpAt) == fallingPitch ? highHz : lowHz;
        std::vector<float> block(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const int absolute = offset + i;
            const double hz = (absolute < jumpAt) == fallingPitch ? highHz : lowHz;
            // Integrate instantaneous frequency so the test signal remains phase-continuous
            // at the jump. A direct sin(2*pi*f*t) switch would create an artificial input
            // discontinuity and incorrectly blame the shifter for it.
            block[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(phase));
            phase += 2.0 * kPi * hz / sr;
        }
        shifter.process(block.data(), n, static_cast<float>(sr / firstHz), 1.2f);
        std::copy(block.begin(), block.end(), output.begin() + offset);
        offset += n;
    }

    TransitionMetrics metrics;
    const int begin = jumpAt - 256;
    const int end = jumpAt + 256;
    for (int i = begin + 1; i < end; ++i)
        metrics.aroundJump = std::max(metrics.aroundJump,
            std::abs(static_cast<double>(output[static_cast<size_t>(i)] - output[static_cast<size_t>(i - 1)])));
    for (int i = 10000; i < 18000; ++i)
        metrics.steady = std::max(metrics.steady,
            std::abs(static_cast<double>(output[static_cast<size_t>(i)] - output[static_cast<size_t>(i - 1)])));
    return metrics;
}

bool runPitchJumpRegressions()
{
    const auto rising = renderPitchJump(false); // 220 -> 330 Hz
    const auto falling = renderPitchJump(true); // 330 -> 220 Hz
    const bool pass = rising.aroundJump < 0.07 && falling.aroundJump < 0.07
                   && rising.steady < 0.07 && falling.steady < 0.07;
    std::printf("fixed-delay @3.50ms: rising jump %.4f / steady %.4f; falling %.4f / steady %.4f %s\n",
                rising.aroundJump, rising.steady, falling.aroundJump, falling.steady,
                pass ? "PASS" : "FAIL");
    return pass;
}



bool runHarmonicBassRegression(double inputHz, int blockSize)
{
    constexpr double sr = 48000.0;
    constexpr int total = 48000;
    constexpr int measureBegin = 12000;
    constexpr int measureEnd = 42000;
    const double ratio = std::pow(2.0, 2.0 / 12.0);
    const double targetHz = inputHz * ratio;

    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(inputHz < 70.0 ? 40.0 : 70.0);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(true);

    std::vector<float> input(static_cast<size_t>(total));
    std::vector<float> output(static_cast<size_t>(total));
    uint32_t noiseState = 0x41c64e6du;
    for (int i = 0; i < total; ++i)
    {
        const double phase = 2.0 * kPi * inputHz * static_cast<double>(i) / sr;
        double sample = 0.0;
        for (int harmonic = 1; harmonic <= 8; ++harmonic)
            sample += (0.29 / static_cast<double>(harmonic)) * std::sin(phase * harmonic);
        noiseState = noiseState * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(noiseState) / 4294967295.0) * 2.0 - 1.0;
        input[static_cast<size_t>(i)] = static_cast<float>(sample + 0.0025 * noise);
    }

    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        std::vector<float> block(input.begin() + offset, input.begin() + offset + n);
        shifter.process(block.data(), n, static_cast<float>(sr / inputHz), static_cast<float>(ratio));
        std::copy(block.begin(), block.end(), output.begin() + offset);
        offset += n;
    }

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
    const double sourceAmplitude = amplitudeAt(inputHz);
    const bool pass = targetAmplitude > 0.08 && targetAmplitude > sourceAmplitude * 2.9;
    std::printf("harmonic bass: %.2f Hz -> %.2f Hz, block %d target/source %.4f / %.4f (% .1fx) %s\n",
                inputHz, targetHz, blockSize, targetAmplitude, sourceAmplitude,
                targetAmplitude / std::max(1.0e-12, sourceAmplitude), pass ? "PASS" : "FAIL");
    return pass;
}

bool runChangingHarmonicBassRegression(int blockSize, double lowHz = 82.4069, double highHz = 98.0)
{
    constexpr double sr = 48000.0;
    constexpr int total = 48000;
    constexpr int jumpAt = total / 2;
    const double ratio = std::pow(2.0, 2.0 / 12.0);
    const double lowTarget = lowHz * ratio;   // F#2
    const double highTarget = highHz * ratio; // A2-ish

    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(40.0);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(true);

    std::vector<float> input(static_cast<size_t>(total));
    std::vector<float> output(static_cast<size_t>(total));
    uint32_t noiseState = 0x73a91b2du;
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double hz = i < jumpAt ? lowHz : highHz;
        double sample = 0.0;
        for (int harmonic = 1; harmonic <= 8; ++harmonic)
            sample += (0.20 / static_cast<double>(harmonic)) * std::sin(phase * harmonic);
        noiseState = noiseState * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(noiseState) / 4294967295.0) * 2.0 - 1.0;
        input[static_cast<size_t>(i)] = static_cast<float>(sample + 0.0015 * noise);
        phase += 2.0 * kPi * hz / sr; // phase-continuous fundamental at the transition
    }

    for (int offset = 0; offset < total;)
    {
        const int n = std::min(blockSize, total - offset);
        std::vector<float> block(input.begin() + offset, input.begin() + offset + n);
        int local = 0;
        while (local < n)
        {
            const int absolute = offset + local;
            const double hz = absolute < jumpAt ? lowHz : highHz;
            // The control parameter represents the pitch estimate currently in effect,
            // not a block-wide average. Split at the known synthetic note boundary so
            // the test's period timeline is the same for every simulated host block.
            int segment = n - local;
            if (absolute < jumpAt)
                segment = std::min(segment, jumpAt - absolute);
            shifter.process(block.data() + local, segment,
                            static_cast<float>(sr / hz), static_cast<float>(ratio));
            local += segment;
        }
        std::copy(block.begin(), block.end(), output.begin() + offset);
        offset += n;
    }

    auto amplitudeAt = [&](double hz, int begin, int end)
    {
        double re = 0.0, im = 0.0;
        for (int i = begin; i < end; ++i)
        {
            const double ph = 2.0 * kPi * hz * static_cast<double>(i) / sr;
            const double x = output[static_cast<size_t>(i)];
            re += x * std::cos(ph);
            im += x * std::sin(ph);
        }
        return 2.0 * std::hypot(re, im) / static_cast<double>(end - begin);
    };

    // Exclude 150 ms after the event so this measures the newly settled note,
    // not the transition crossfade or stale pre-jump grain energy.
    const int lowBegin = static_cast<int>(sr * 0.12);
    const int lowEnd = static_cast<int>(sr * 0.42);
    const int highBegin = static_cast<int>(sr * 0.68);
    const int highEnd = static_cast<int>(sr * 0.94);
    const double lowTargetAmp = amplitudeAt(lowTarget, lowBegin, lowEnd);
    const double lowSourceAmp = amplitudeAt(lowHz, lowBegin, lowEnd);
    const double highTargetAmp = amplitudeAt(highTarget, highBegin, highEnd);
    const double highSourceAmp = amplitudeAt(highHz, highBegin, highEnd);

    double maxStep = 0.0;
    const int aroundBegin = std::max(1, jumpAt - 1024);
    const int aroundEnd = std::min(total, jumpAt + 1024);
    for (int i = aroundBegin; i < aroundEnd; ++i)
        maxStep = std::max(maxStep, std::abs(static_cast<double>(output[static_cast<size_t>(i)]
                                                               - output[static_cast<size_t>(i - 1)])));

    // At E1, a short fixed-delay window leaves more original fundamental energy than
    // at E2/E3, so use a 1.5x floor there rather than pretending the shift is as clean.
    const double requiredDominance = lowHz < 70.0 ? 1.5 : 2.0;
    const bool pass = lowTargetAmp > 0.04 && lowTargetAmp > lowSourceAmp * requiredDominance
                   && highTargetAmp > 0.04 && highTargetAmp > highSourceAmp * requiredDominance
                   && maxStep < 0.12;
    std::printf("changing harmonic bass block %d: %.2f->%.2f Hz target/source %.4f/%.4f and %.4f/%.4f, max-step %.4f %s\n",
                blockSize, lowHz, highHz, lowTargetAmp, lowSourceAmp, highTargetAmp, highSourceAmp,
                maxStep, pass ? "PASS" : "FAIL");
    return pass;
}

bool runFastResetNoStaleAudioRegression()
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 127;
    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(70.0);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(true);

    // Prime both rings with voiced audio and a non-unity ratio so the output ring
    // contains pending grain energy immediately before each reset.
    int64_t sample = 0;
    for (int blockIndex = 0; blockIndex < 80; ++blockIndex)
    {
        std::vector<float> block(static_cast<size_t>(blockSize));
        for (int i = 0; i < blockSize; ++i, ++sample)
            block[static_cast<size_t>(i)] = 0.3f * static_cast<float>(
                std::sin(2.0 * kPi * 220.0 * static_cast<double>(sample) / sr));
        shifter.process(block.data(), blockSize, static_cast<float>(sr / 220.0), 1.2f);
    }

    double maxStaleOutput = 0.0;
    const bool modes[] = { false, true, false, true };
    for (const bool mode : modes)
    {
        shifter.setLowLatencyExperiment(mode);
        for (int blockIndex = 0; blockIndex < 40; ++blockIndex)
        {
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            shifter.process(block.data(), blockSize, static_cast<float>(sr / 220.0), 1.2f);
            for (const float x : block)
                maxStaleOutput = std::max(maxStaleOutput, std::abs(static_cast<double>(x)));
        }
    }

    const bool pass = maxStaleOutput < 1.0e-7;
    std::printf("fast reset: repeated mode switches after active audio, zero-input max %.9f %s\n",
                maxStaleOutput, pass ? "PASS" : "FAIL");
    return pass;
}

int main()
{
    constexpr double sr = 48000.0;
    const double ratio = std::pow(2.0, 3.0 / 12.0);
    const auto a = render(220.0, ratio, sr, 64, true);
    const auto b = render(220.0, ratio, sr, 256, true);
    const auto c = render(110.0, 1.0, sr, 127, true);
    const auto formantUp = render(220.0, ratio, sr, 127, true, false, 100.0, 3.5, 4.0);
    const auto formantDown = render(220.0, ratio, sr, 127, true, false, 100.0, 3.5, -4.0);
    const auto subBassStandard = render(41.2035, std::pow(2.0, 2.0 / 12.0), sr, 127, true, false, 40.0);
    const auto lowLatency = render(220.0, ratio, sr, 64, true, true);
    const auto lowLatencyBass = render(82.41, std::pow(2.0, 2.0 / 12.0), sr, 127, true, true, 70.0);
    const auto lowLatency441 = render(220.0, ratio, 44100.0, 128, true, true);
    const auto sub2 = render(220.0, ratio, sr, 64, true, true, 100.0, 1.9);
    const auto sub2Bass = render(82.41, std::pow(2.0, 2.0 / 12.0), sr, 127, true, true, 70.0, 1.9);
    const auto sub2_441 = render(220.0, ratio, 44100.0, 128, true, true, 100.0, 1.9);
    const auto oneMs = render(220.0, ratio, sr, 64, true, true, 100.0, 1.0);
    const auto oneMsBass = render(82.41, std::pow(2.0, 2.0 / 12.0), sr, 127, true, true, 70.0, 1.0);
    const auto oneMs441 = render(220.0, ratio, 44100.0, 128, true, true, 100.0, 1.0);
    const auto point75 = render(220.0, ratio, sr, 64, true, true, 100.0, 0.75);
    const auto point75Bass = render(82.41, std::pow(2.0, 2.0 / 12.0), sr, 127, true, true, 70.0, 0.75);
    const auto point75_441 = render(220.0, ratio, 44100.0, 128, true, true, 100.0, 0.75);
    const double expected = 220.0 * ratio;
    std::printf("pitch-shifter: target %.2f Hz; 64-frame %.2f Hz RMS %.4f; 256-frame %.2f Hz RMS %.4f; unity-bass %.2f Hz RMS %.4f; latency %d samples (%.2f ms)\n",
                expected, a.frequency, a.rms, b.frequency, b.rms, c.frequency, c.rms,
                a.latency, 1000.0 * a.latency / sr);

    const bool pitchOk = std::abs(a.frequency - expected) < 3.0 && std::abs(b.frequency - expected) < 3.0;
    const bool blockOk = std::abs(a.frequency - b.frequency) < 1.0;
    const bool levelOk = a.rms > 0.04 && a.rms < 0.6 && b.rms > 0.04 && b.rms < 0.6;
    const bool independentFormantShiftOk = std::abs(formantUp.frequency - expected) < 3.0
        && std::abs(formantDown.frequency - expected) < 3.0
        && formantUp.rms > 0.04 && formantUp.rms < 0.6
        && formantDown.rms > 0.04 && formantDown.rms < 0.6
        && std::isfinite(formantUp.rms) && std::isfinite(formantDown.rms);
    std::printf("independent formant shift +/-4 st: pitch %.2f / %.2f Hz (target %.2f), RMS %.4f / %.4f -> %s\n",
                formantUp.frequency, formantDown.frequency, expected, formantUp.rms, formantDown.rms,
                independentFormantShiftOk ? "PASS" : "FAIL");
    const bool bassOk = std::abs(c.frequency - 110.0) < 2.0 && c.rms > 0.04 && c.rms < 0.6;
    const bool subBassStandardOk = std::abs(subBassStandard.frequency - 41.2035 * std::pow(2.0, 2.0 / 12.0)) < 3.0
                                && subBassStandard.latency == static_cast<int>(std::ceil(1.3 * std::ceil(sr / 40.0))) + 8
                                && subBassStandard.rms > 0.04 && subBassStandard.rms < 0.6;
    std::printf("standard Sub Bass: E1->F#1 %.2f Hz (target %.2f), latency %d samples %.2f ms %s\n",
                subBassStandard.frequency, 41.2035 * std::pow(2.0, 2.0 / 12.0),
                subBassStandard.latency, 1000.0 * subBassStandard.latency / sr,
                subBassStandardOk ? "PASS" : "FAIL");
    const bool lowLatencyBudgetOk = lowLatency.latency <= static_cast<int>(sr * 0.004)
                                 && lowLatencyBass.latency <= static_cast<int>(sr * 0.004)
                                 && lowLatency441.latency <= static_cast<int>(44100.0 * 0.004);
    const bool lowLatencyPitchOk = std::abs(lowLatency.frequency - expected) < 3.0
                                && std::abs(lowLatencyBass.frequency - 82.41 * std::pow(2.0, 2.0 / 12.0)) < 3.0;
    const bool lowLatencyLevelOk = lowLatency.rms > 0.04 && lowLatency.rms < 0.6
                                && lowLatencyBass.rms > 0.04 && lowLatencyBass.rms < 0.6;
    const bool lowLatency441PitchOk = std::abs(lowLatency441.frequency - expected) < 3.0;
    std::printf("experimental fixed-delay: 220 Hz->%.2f Hz RMS %.4f; 82.41 Hz bass->%.2f Hz RMS %.4f; 48 kHz %d samples (%.2f ms); 44.1 kHz %d samples (%.2f ms)\n",
                lowLatency.frequency, lowLatency.rms, lowLatencyBass.frequency, lowLatencyBass.rms,
                lowLatency.latency, 1000.0 * lowLatency.latency / sr,
                lowLatency441.latency, 1000.0 * lowLatency441.latency / 44100.0);
    const bool sub2BudgetOk = sub2.latency < static_cast<int>(sr * 0.002)
                           && sub2Bass.latency < static_cast<int>(sr * 0.002)
                           && sub2_441.latency < static_cast<int>(44100.0 * 0.002);
    const bool sub2PitchOk = std::abs(sub2.frequency - expected) < 3.0
                          && std::abs(sub2Bass.frequency - 82.41 * std::pow(2.0, 2.0 / 12.0)) < 3.0
                          && std::abs(sub2_441.frequency - expected) < 3.0;
    std::printf("sub-2ms verification: 48 kHz %d samples (%.2f ms), bass %.2f Hz, 44.1 kHz %d samples (%.2f ms); steady-tone %s\n",
                sub2.latency, 1000.0 * sub2.latency / sr, sub2Bass.frequency,
                sub2_441.latency, 1000.0 * sub2_441.latency / 44100.0,
                sub2BudgetOk && sub2PitchOk ? "PASS" : "FAIL");
    const bool oneMsOk = oneMs.latency < static_cast<int>(sr * 0.002)
        && oneMsBass.latency < static_cast<int>(sr * 0.002)
        && oneMs441.latency < static_cast<int>(44100.0 * 0.002)
        && std::abs(oneMs.frequency - expected) < 3.0
        && std::abs(oneMsBass.frequency - 82.41 * std::pow(2.0, 2.0 / 12.0)) < 3.0
        && std::abs(oneMs441.frequency - expected) < 3.0
        && oneMs.rms > 0.04 && oneMsBass.rms > 0.04 && oneMs441.rms > 0.04;
    std::printf("sub-1ms steady-tone: 48 kHz %.2f Hz; E2 shift %.2f Hz; 44.1 kHz %.2f Hz -> %s\n",
                oneMs.frequency, oneMsBass.frequency, oneMs441.frequency, oneMsOk ? "PASS" : "QUALITY FAIL");
    const bool point75Ok = point75.latency < static_cast<int>(sr * 0.002)
        && point75Bass.latency < static_cast<int>(sr * 0.002)
        && point75_441.latency < static_cast<int>(44100.0 * 0.002)
        && std::abs(point75.frequency - expected) < 3.0
        && std::abs(point75Bass.frequency - 82.41 * std::pow(2.0, 2.0 / 12.0)) < 3.0
        && std::abs(point75_441.frequency - expected) < 3.0
        && point75.rms > 0.04 && point75Bass.rms > 0.04 && point75_441.rms > 0.04;
    std::printf("0.75ms steady-tone: 48 kHz %.2f Hz; E2 shift %.2f Hz; 44.1 kHz %.2f Hz -> %s\n",
                point75.frequency, point75Bass.frequency, point75_441.frequency, point75Ok ? "PASS" : "QUALITY FAIL");
    const bool jumpOk = runPitchJumpRegressions();
    const bool resetOk = runFastResetNoStaleAudioRegression();
    const bool formantEnvelopeOk = runIndependentFormantEnvelopeRegression();
    bool harmonicBassOk = true;
    for (const double bassHz : { 41.2035, 82.4069, 164.8138 })
        for (const int bassBlockSize : { 32, 64, 127, 256, 1024 })
            harmonicBassOk = runHarmonicBassRegression(bassHz, bassBlockSize) && harmonicBassOk;
    bool changingBassOk = true;
    for (const int bassBlockSize : { 32, 64, 127, 256, 1024 })
    {
        changingBassOk = runChangingHarmonicBassRegression(bassBlockSize) && changingBassOk; // E2 -> G2
        changingBassOk = runChangingHarmonicBassRegression(bassBlockSize, 41.2035, 49.0) && changingBassOk; // E1 -> G1
    }
    const bool lagOk = runFixedDelayLagRegressions();
    const auto sub2Rise = renderPitchJump(false, 1.9);
    const auto sub2Fall = renderPitchJump(true, 1.9);
    const auto sub2BassRise = renderPitchJump(false, 1.9, 82.41, 110.0);
    const auto sub2BassFall = renderPitchJump(true, 1.9, 82.41, 110.0);
    const bool sub2TransitionsOk = sub2Rise.aroundJump < 0.07 && sub2Fall.aroundJump < 0.07
                                && sub2BassRise.aroundJump < 0.07 && sub2BassFall.aroundJump < 0.07
                                && sub2Rise.steady < 0.07 && sub2Fall.steady < 0.07
                                && sub2BassRise.steady < 0.07 && sub2BassFall.steady < 0.07;
    std::printf("sub-2ms @1.90ms: mid jump %.4f/%.4f steady %.4f/%.4f; bass jump %.4f/%.4f steady %.4f/%.4f -> %s\n",
                sub2Rise.aroundJump, sub2Fall.aroundJump, sub2Rise.steady, sub2Fall.steady,
                sub2BassRise.aroundJump, sub2BassFall.aroundJump, sub2BassRise.steady, sub2BassFall.steady,
                sub2TransitionsOk ? "PASS" : "FAIL");
    const auto oneMsRise = renderPitchJump(false, 1.0);
    const auto oneMsFall = renderPitchJump(true, 1.0);
    const auto oneMsBassRise = renderPitchJump(false, 1.0, 82.41, 110.0);
    const auto oneMsBassFall = renderPitchJump(true, 1.0, 82.41, 110.0);
    const bool oneMsTransitionOk = oneMsRise.aroundJump < 0.07 && oneMsFall.aroundJump < 0.07
                      && oneMsBassRise.aroundJump < 0.07 && oneMsBassFall.aroundJump < 0.07
                      && oneMsRise.steady < 0.07 && oneMsFall.steady < 0.07
                      && oneMsBassRise.steady < 0.07 && oneMsBassFall.steady < 0.07;
    std::printf("sub-1ms @1.00ms: mid jump %.4f/%.4f steady %.4f/%.4f; bass jump %.4f/%.4f steady %.4f/%.4f -> %s\n",
                oneMsRise.aroundJump, oneMsFall.aroundJump, oneMsRise.steady, oneMsFall.steady,
                oneMsBassRise.aroundJump, oneMsBassFall.aroundJump, oneMsBassRise.steady, oneMsBassFall.steady,
                oneMsTransitionOk ? "PASS" : "QUALITY FAIL");
    const auto point75Rise = renderPitchJump(false, 0.75);
    const auto point75Fall = renderPitchJump(true, 0.75);
    const auto point75BassRise = renderPitchJump(false, 0.75, 82.41, 110.0);
    const auto point75BassFall = renderPitchJump(true, 0.75, 82.41, 110.0);
    const bool point75TransitionOk = point75Rise.aroundJump < 0.07 && point75Fall.aroundJump < 0.07
                        && point75BassRise.aroundJump < 0.07 && point75BassFall.aroundJump < 0.07
                        && point75Rise.steady < 0.07 && point75Fall.steady < 0.07
                        && point75BassRise.steady < 0.07 && point75BassFall.steady < 0.07;
    std::printf("sub-1ms @0.75ms: mid jump %.4f/%.4f steady %.4f/%.4f; bass jump %.4f/%.4f steady %.4f/%.4f -> %s\n",
                point75Rise.aroundJump, point75Fall.aroundJump, point75Rise.steady, point75Fall.steady,
                point75BassRise.aroundJump, point75BassFall.aroundJump, point75BassRise.steady, point75BassFall.steady,
                point75TransitionOk ? "PASS" : "QUALITY FAIL");
    // Sub-2ms is the release research target. The sub-1ms sweep is diagnostic only and
    // does not gate the suite; it is recorded honestly as either pass or quality failure.
    return pitchOk && blockOk && levelOk && bassOk && subBassStandardOk && independentFormantShiftOk && lowLatencyBudgetOk
        && lowLatencyPitchOk && lowLatencyLevelOk && lowLatency441PitchOk && jumpOk && lagOk
        && sub2BudgetOk && sub2PitchOk && sub2TransitionsOk && oneMsOk && point75Ok && resetOk && formantEnvelopeOk && harmonicBassOk && changingBassOk ? 0 : 1;
}
