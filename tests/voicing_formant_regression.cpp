#include "PitchShifter.h"
#include "AutoTuneEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
struct Metrics { double noiseErrorDb = 0.0, voicedToDryRms = 0.0, peak = 0.0, rms = 0.0; int latency = 0; };

Metrics run (double sr, int blockSize, bool lowLatency)
{
    humtune::PitchShifter s;
    s.prepare(sr, blockSize);
    s.setMinFrequency(100.0);
    s.setLowLatencyExperiment(lowLatency);
    s.setLowLatencyBudgetMilliseconds(1.9);
    s.setFormantPreserve(true);
    const int n = static_cast<int>(sr * 1.15);
    const int consonantStart = static_cast<int>(sr * 0.35);
    const int consonantEnd = static_cast<int>(sr * 0.70);
    std::vector<float> input(n), output(n);
    uint32_t seed = 0x5ec1234u;
    for (int i = 0; i < n; ++i)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const double noise = static_cast<double>(seed & 0xffffu) / 32768.0 - 1.0;
        const double phase = 2.0 * pi * 228.0 * i / sr;
        input[i] = static_cast<float>(i >= consonantStart && i < consonantEnd
            ? 0.23 * noise : 0.19 * std::sin(phase) + 0.06 * std::sin(2 * phase));
    }

    int at = 0;
    while (at < n)
    {
        const int boundary = at < consonantStart ? consonantStart
            : (at < consonantEnd ? consonantEnd : n);
        const int sz = std::min(blockSize, boundary - at);
        s.setVoiced(at < consonantStart || at >= consonantEnd);
        std::vector<float> block(input.begin() + at, input.begin() + at + sz);
        s.process(block.data(), sz, static_cast<float>(sr / 228.0), 220.0f / 228.0f);
        std::copy(block.begin(), block.end(), output.begin() + at);
        at += sz;
    }
    Metrics m;
    m.latency = s.getLatencySamples();
    // After the ramp has settled, the unvoiced segment must reproduce the
    // exact latency-aligned original noise (not grain-correlated false pitch).
    const int first = consonantStart + m.latency + static_cast<int>(0.040 * sr);
    const int last = consonantEnd - static_cast<int>(0.020 * sr);
    double noiseDifference = 0.0, noiseReference = 0.0;
    for (int i = first; i < last; ++i)
    {
        const double d = output[i] - input[i - m.latency];
        noiseDifference += d*d;
        noiseReference += double(input[i - m.latency])*input[i - m.latency];
    }
    m.noiseErrorDb = 10.0 * std::log10((noiseDifference + 1.e-25) / (noiseReference + 1.e-25));
    for (int i = static_cast<int>(sr * 0.9); i < n; ++i)
    {
        const double d = output[i] - input[i - m.latency];
        m.voicedToDryRms += d * d;
        m.rms += output[i]*output[i];
        m.peak = std::max(m.peak, std::abs(static_cast<double>(output[i])));
    }
    m.voicedToDryRms = std::sqrt(m.voicedToDryRms / (n - static_cast<int>(sr * .9)));
    m.rms = std::sqrt(m.rms / (n - static_cast<int>(sr * .9)));
    return m;
}
}
int main()
{
    bool ok = true;
    for (double sr : {44100.0, 48000.0})
        for (bool low : {false, true})
            for (int block : {32, 127, 512})
            {
                const auto m = run(sr, block, low);
                const bool pass = m.noiseErrorDb < -72.0 && m.voicedToDryRms > .005
                               && m.rms > .025 && m.peak < 1.0;
                std::printf("voicing sr=%.0f low=%d block=%d delay=%d noise error=%.1f dB voiced diff=%.4f rms=%.4f peak=%.4f %s\n",
                            sr, low, block, m.latency, m.noiseErrorDb, m.voicedToDryRms,
                            m.rms, m.peak, pass ? "PASS" : "FAIL");
                ok &= pass;
            }
    // A singer who is already on the correct note should retain the exact
    // delayed source rather than picking up PSOLA coloration at a unity ratio.
    for (bool low : {false, true})
    {
        constexpr double sr = 48000.0;
        constexpr int n = 48000;
        humtune::AutoTuneEngine engine;
        engine.setVoiceRange(2);
        engine.setLowLatencyMode(low);
        engine.setRootNote(0);
        engine.setScaleType(0);
        engine.prepare(sr, 127);
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            in[i] = static_cast<float>(0.2 * std::sin(2 * pi * 220 * t)
                      + 0.05 * std::sin(4 * pi * 220 * t));
        }
        for (int off = 0; off < n; off += 127)
        {
            const int len = std::min(127, n-off);
            juce::AudioBuffer<float> block(1, len);
            std::copy(in.begin() + off, in.begin() + off + len, block.getWritePointer(0));
            engine.process(block);
            std::copy(block.getReadPointer(0), block.getReadPointer(0) + len, out.begin() + off);
        }
        const int latency = engine.getLatencySamples();
        double error = 0.0, reference = 0.0;
        for (int i = 24000; i < n; ++i)
        {
            const double d = out[i] - in[i-latency];
            error += d*d;
            reference += double(in[i-latency])*in[i-latency];
        }
        const double errorDb = 10 * std::log10((error + 1.e-25) / (reference + 1.e-25));
        const bool pass = errorDb < -65;
        std::printf("engine unity transparency low=%d delay=%d error=%.2f dB %s\n",
                    low, latency, errorDb, pass ? "PASS" : "FAIL");
        ok &= pass;
    }

    // Formant-preservation automation uses globally aligned control updates,
    // not one update per host callback. Compare different DAW buffer sizes.
    auto renderFormantToggle = [](int blockSize)
    {
        humtune::PitchShifter shifter;
        shifter.prepare(48000, blockSize);
        shifter.setMinFrequency(100.0);
        shifter.setFormantPreserve(true);
        constexpr int total = 48000;
        constexpr int toggleAt = 13517;
        std::vector<float> output(total);
        int at = 0;
        while (at < total)
        {
            const int next = std::min(total, at < toggleAt ? toggleAt : total);
            const int n = std::min(blockSize, next - at);
            if (at >= toggleAt) shifter.setFormantPreserve(false);
            std::vector<float> audio(n);
            for (int i = 0; i < n; ++i)
            {
                const double phase = 2.0 * pi * 235.0 * (at+i) / 48000;
                audio[i] = static_cast<float>(0.15 * std::sin(phase) + 0.05 * std::sin(2*phase));
            }
            shifter.process(audio.data(), n, static_cast<float>(48000.0 / 235), 1.12f);
            std::copy(audio.begin(), audio.end(), output.begin() + at);
            at += n;
        }
        return output;
    };
    const auto referenceRender = renderFormantToggle(32);
    for (int blockSize : {127, 256, 511})
    {
        const auto candidate = renderFormantToggle(blockSize);
        double e = 0.0, energy = 0.0;
        for (size_t i = 0; i < candidate.size(); ++i)
        {
            const double d = candidate[i] - referenceRender[i];
            e += d*d;
            energy += referenceRender[i]*referenceRender[i];
        }
        const double relative = std::sqrt(e / std::max(energy, 1.e-20));
        const bool pass = relative < 1.e-4;
        std::printf("formant toggle block=%d relative RMS delta=%.9g %s\n", blockSize,
                    relative, pass ? "PASS" : "FAIL");
        ok &= pass;
    }

    // Formant automation remains finite and bounded under extreme toggling.
    humtune::PitchShifter shifter;
    shifter.prepare(48000, 127);
    shifter.setMinFrequency(80);
    int bad = 0;
    for (int block = 0; block < 1000; ++block)
    {
        if (block % 4 == 0) shifter.setFormantPreserve((block / 4) % 2 == 0);
        if (block % 5 == 0) shifter.setFormantShiftSemitones((block / 5) % 2 == 0 ? -4.0f : 4.0f);
        float audio[127];
        for (int i = 0; i < 127; ++i)
        {
            const double phase = 2.0 * pi * 185.0 * (block*127+i) / 48000;
            audio[i] = static_cast<float>(0.15 * std::sin(phase) + 0.05 * std::sin(2 * phase));
        }
        shifter.process(audio, 127, static_cast<float>(48000.0 / 185), 1.17f);
        for (float x : audio) if (!std::isfinite(x) || std::abs(x) > 4.0f) ++bad;
    }
    std::printf("formant automation nonfinite/excessive samples=%d %s\n", bad, bad == 0 ? "PASS" : "FAIL");
    return ok && bad == 0 ? 0 : 1;
}
