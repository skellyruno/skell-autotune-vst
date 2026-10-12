#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace humtune
{

class PitchShifter
{
public:
    static constexpr double kMinHz = 70.0;       // lowest pitch ever supported (buffers are sized for it)

    // The fixed delay, measured in periods of the LOWEST pitch being tracked. It used to be 2.5:
    // the shifter waited until real audio had arrived for the whole length of every grain.
    // Now it only waits for part of it and "guesses" the rest by repeating the previous
    // cycle of the voice (see processChunk), which is why a much smaller number works.
    // Measured on realistic voices: 1.6 keeps the output within about 0.4% of the exact result;
    // going below about 1.4 starts to cost real quality.
    static constexpr double kLatencyPeriods = 1.6;
    static constexpr double kMaxHz = 1500.0;

    void prepare (double sampleRate, int /*blockSize*/)
    {
        sr = sampleRate;

        const int maxPeriodAlloc = std::max(32, static_cast<int>(std::ceil(sr / kMinHz)));
        const int latencyAlloc   = static_cast<int>(std::ceil(kLatencyPeriods * maxPeriodAlloc)) + 8;

        inSize  = nextPow2(latencyAlloc + 4 * maxPeriodAlloc + 2 * kChunk);
        outSize = nextPow2(2 * latencyAlloc + 10 * maxPeriodAlloc + 4 * kChunk);
        inMask  = static_cast<int64_t>(inSize - 1);
        outMask = static_cast<int64_t>(outSize - 1);

        inRing.assign(static_cast<size_t>(inSize), 0.0f);
        outRing.assign(static_cast<size_t>(outSize), 0.0f);

        minPeriod = std::max(4, static_cast<int>(std::floor(sr / kMaxHz)));
        setMinFrequency(kMinHz);
    }

    void setMinFrequency (double hz)
    {
        hz        = juce::jlimit(kMinHz, 400.0, hz);
        maxPeriod = std::max(32, static_cast<int>(std::ceil(sr / hz)));
        latency   = static_cast<int>(std::ceil(kLatencyPeriods * maxPeriod)) + 8;
        reset();
    }

    void setFormantPreserve (bool on) { formantPreserve = on; }

    void reset()
    {
        std::fill(inRing.begin(), inRing.end(), 0.0f);
        std::fill(outRing.begin(), outRing.end(), 0.0f);
        inCount  = 0;
        outRead  = 0;
        nextTs   = 0.0;
        markPos  = -static_cast<double>(latency);
        lastPeriod = juce::jlimit(static_cast<double>(minPeriod),
                                  static_cast<double>(maxPeriod), sr / 200.0);
    }

    int getLatencySamples() const { return latency; }

    void process (float* data, int numSamples, float periodSamples, float shiftRatio)
    {
        if (numSamples <= 0 || inRing.empty())
            return;

        double periodF;
        if (std::isfinite(periodSamples) && periodSamples >= static_cast<float>(minPeriod))
        {
            periodF = juce::jlimit(static_cast<double>(minPeriod), static_cast<double>(maxPeriod),
                                   static_cast<double>(periodSamples));
            lastPeriod = periodF;
        }
        else
        {
            // No pitch (breath, consonants, silence): use short grains. They never need
            // guessed audio, so such sounds pass through unchanged.
            periodF = juce::jlimit(static_cast<double>(minPeriod), static_cast<double>(maxPeriod),
                                   std::min(lastPeriod, 0.006 * sr));
        }

        const double ratio = std::isfinite(shiftRatio)
                           ? juce::jlimit(0.6, 1.8, static_cast<double>(shiftRatio))
                           : 1.0;

        for (int pos = 0; pos < numSamples; pos += kChunk)
            processChunk(data + pos, std::min(kChunk, numSamples - pos), periodF, ratio);
    }

    bool isNearUnity (float ratio) const
    {
        return std::abs(ratio - 1.0f) < 0.002f;
    }

private:
    static constexpr int kChunk = 256;

    static int nextPow2 (int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void processChunk (float* data, int m, double periodF, double ratio)
    {
        for (int i = 0; i < m; ++i)
            inRing[static_cast<size_t>((inCount + i) & inMask)] = data[i];
        inCount += m;

        // Output spacing of the grains (this sets the new pitch)
        const double hop    = periodF / ratio;
        const double margin = 8.0;

        // Every grain covers exactly one period of the voice on each side of its centre.
        // That length matters: it puts the zeros of the window's spectrum exactly on the
        // voice's harmonics, so nothing leaks into the output (any other length adds
        // low-frequency rumble when the pitch is changed).
        //   formants kept : the grain is read at normal speed  -> output half-window = period
        //   formants moved: the grain is read at `ratio` speed -> output half-window = hop
        double wo = formantPreserve ? periodF : hop;

        // Longest half-window the delay budget allows, keeping a little real audio ahead.
        // Only the very lowest notes get anywhere near this limit.
        const double woMax = static_cast<double>(latency) - margin - 0.6 * periodF;
        wo = std::max(std::min(wo, woMax), 0.5 * periodF);

        const double rate = periodF / wo;                      // source samples per output sample
        const float  gain = static_cast<float>(hop / wo);      // keeps the loudness constant
        const float  piOverW = juce::MathConstants<float>::pi / static_cast<float>(wo);

        // How much REAL audio must exist beyond the mark before a grain is built. Whatever
        // is still missing at the far end of the grain is guessed (see below).
        const double xCap  = static_cast<double>(latency) - margin - 0.5 * periodF - wo;
        const double xNeed = std::min(periodF, std::max(0.0, xCap));

        for (;;)
        {
            const double ta0 = nextTs - static_cast<double>(latency);

            // Pick the analysis mark (spaced one period apart) nearest to ta0
            while (markPos + 0.5 * periodF < ta0)
                markPos += periodF;

            if (static_cast<int64_t>(std::floor(markPos + xNeed)) + 3 >= inCount)
                break;                                          // wait for more audio

            const double  limit  = static_cast<double>(inCount) - 3.0;   // newest position the cubic can use
            const int64_t oFirst = static_cast<int64_t>(std::ceil (nextTs - wo));
            const int64_t oLast  = static_cast<int64_t>(std::floor(nextTs + wo));

            for (int64_t o = std::max(oFirst, outRead); o <= oLast; ++o)
            {
                const float x = static_cast<float>(static_cast<double>(o) - nextTs + wo);
                if (x <= 0.0f)
                    continue;
                const float w = 0.5f * (1.0f - std::cos(piOverW * x));      // Hann window

                double pos = markPos + (static_cast<double>(o) - nextTs) * rate;
                if (pos > limit)        // not arrived yet: repeat the previous cycle of the voice
                    pos -= std::ceil((pos - limit) / periodF) * periodF;

                const double  fl = std::floor(pos);
                const int64_t a  = static_cast<int64_t>(fl);
                if (a < 1)
                    continue;

                const float t  = static_cast<float>(pos - fl);
                const float t2 = t * t, t3 = t2 * t;
                const float c0 = -0.5f * t3 + t2 - 0.5f * t;
                const float c1 =  1.5f * t3 - 2.5f * t2 + 1.0f;
                const float c2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
                const float c3 =  0.5f * t3 - 0.5f * t2;

                const float smp = c0 * inRing[static_cast<size_t>((a - 1) & inMask)]
                                + c1 * inRing[static_cast<size_t>( a      & inMask)]
                                + c2 * inRing[static_cast<size_t>((a + 1) & inMask)]
                                + c3 * inRing[static_cast<size_t>((a + 2) & inMask)];

                outRing[static_cast<size_t>(o & outMask)] += smp * w * gain;
            }

            nextTs += hop;
        }

        for (int i = 0; i < m; ++i)
        {
            const size_t idx = static_cast<size_t>((outRead + i) & outMask);
            data[i] = outRing[idx];
            outRing[idx] = 0.0f;
        }
        outRead += m;
    }

    double sr = 44100.0;
    int maxPeriod = 800, minPeriod = 32, latency = 2000;
    int inSize = 0, outSize = 0;
    int64_t inMask = 0, outMask = 0;

    std::vector<float> inRing, outRing;

    int64_t inCount = 0;
    int64_t outRead = 0;
    double  nextTs  = 0.0;
    double  markPos = 0.0;
    double  lastPeriod = 220.0;
    bool    formantPreserve = true;
};

} // namespace humtune
