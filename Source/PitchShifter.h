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
    static constexpr double kMinHz = 35.0;       // buffers support low bass down to E1 and below

    // The fixed delay, measured in periods of the LOWEST pitch being tracked. It used to be 2.5:
    // the shifter waited until real audio had arrived for the whole length of every grain.
    // Now it only waits for part of it and "guesses" the rest by repeating the previous
    // cycle of the voice (see processChunk), which is why a much smaller number works.
    // Legacy period-bound mode retains this conservative setting until real-vocal
    // comparisons justify a different default. The experimental mode uses its own budget.
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
        outRingEpoch.assign(static_cast<size_t>(outSize), 0);
        ringEpoch = 0;

        minPeriod = std::max(4, static_cast<int>(std::floor(sr / kMaxHz)));
        setMinFrequency(kMinHz);
    }

    void setMinFrequency (double hz)
    {
        hz        = juce::jlimit(kMinHz, 400.0, hz);
        maxPeriod = std::max(32, static_cast<int>(std::ceil(sr / hz)));
        updateLatency();
        reset();
    }

    void setFormantPreserve (bool on) { formantPreserve = on; }

    // Experimental fixed-budget path for evaluating predictive cycle resynthesis.
    // This is intentionally opt-in until the regression suite proves quality.
    void setLowLatencyExperiment (bool on)
    {
        if (lowLatencyExperiment == on)
            return;
        lowLatencyExperiment = on;
        updateLatency();
        reset();
    }

    // Set the experimental budget for offline/QA sweeps. The class defaults to 3.5 ms;
    // AutoTuneEngine selects 1.9 ms only when the explicit experimental switch is enabled.
    // Lower values are stress-test configurations until real-vocal quality is validated.
    void setLowLatencyBudgetMilliseconds (double milliseconds)
    {
        lowLatencyBudgetMs = juce::jlimit(0.75, 3.5, std::isfinite(milliseconds) ? milliseconds : 3.5);
        updateLatency();
        reset();
    }

    void updateLatency()
    {
        latency = lowLatencyExperiment
                ? std::max(1, static_cast<int>(std::ceil(sr * lowLatencyBudgetMs * 0.001)))
                : static_cast<int>(std::ceil(kLatencyPeriods * maxPeriod)) + 8;
    }

    void reset()
    {
        // Avoid clearing the full audio rings when a host automates Low Latency or
        // Voice Range. Ring memory is preallocated in prepare(); epochs make old
        // output slots logically empty after O(1) reset. Input history becomes valid
        // only as new samples advance inCount from zero, so stale input-ring cells are
        // not addressable by the reset timeline.
        ++ringEpoch;
        if (ringEpoch == 0) // practically unreachable; preserve correctness on wrap.
        {
            std::fill(outRingEpoch.begin(), outRingEpoch.end(), 0);
            ringEpoch = 1;
        }
        inCount  = 0;
        outRead  = 0;
        nextTs   = 0.0;
        markPos  = -static_cast<double>(latency);
        lastPeriod = juce::jlimit(static_cast<double>(minPeriod),
                                  static_cast<double>(maxPeriod), sr / 200.0);
        smoothedPeriod = lastPeriod;
        lowLatencySlicePeriod = lastPeriod;
        lastInputPeriod = lastPeriod;
        transitionBlendAge = -1;
        transitionBlendTotal = 0;
    }

    int getLatencySamples() const { return latency; }

    void process (float* data, int numSamples, float periodSamples, float shiftRatio)
    {
        if (numSamples <= 0 || inRing.empty())
            return;

        double targetPeriod;
        if (std::isfinite(periodSamples) && periodSamples >= static_cast<float>(minPeriod))
        {
            targetPeriod = juce::jlimit(static_cast<double>(minPeriod), static_cast<double>(maxPeriod),
                                        static_cast<double>(periodSamples));
            lastPeriod = targetPeriod;
        }
        else
        {
            targetPeriod = juce::jlimit(static_cast<double>(minPeriod), static_cast<double>(maxPeriod),
                                        std::min(lastPeriod, 0.006 * sr));
        }
        if (lowLatencyExperiment)
        {
            // Detect a real note transition, then crossfade smoothly toward latency-aligned
            // input for transient preservation before returning to resynthesis. The short
            // attack avoids introducing a new discontinuity at the blend boundary.
            if (lastInputPeriod > 0.0
                && std::abs(targetPeriod - lastInputPeriod) / lastInputPeriod > 0.05)
            {
                transitionBlendTotal = std::max(1, static_cast<int>(std::ceil(sr * 0.016)));
                transitionBlendAge = 0;
            }
            lastInputPeriod = targetPeriod;
        }

        const double ratio = std::isfinite(shiftRatio)
                           ? juce::jlimit(0.6, 1.8, static_cast<double>(shiftRatio))
                           : 1.0;

        if (lowLatencyExperiment)
        {
            // Update the predictor period on a persistent 32-sample timeline, rather
            // than once per host call. The DSP work below is still scheduled sample by
            // sample, so decisions are causal and callback-size invariant; holding the
            // period geometry for each globally aligned slice avoids the bass instability
            // seen when moving the grain spacing by tiny amounts every sample.
            const double smoothingCoeff = std::exp(-static_cast<double>(kLowLatencyChunk)
                                                   / (sr * 0.0025));
            for (int i = 0; i < numSamples; ++i)
            {
                if ((inCount % kLowLatencyChunk) == 0)
                {
                    smoothedPeriod = smoothingCoeff * smoothedPeriod
                                   + (1.0 - smoothingCoeff) * targetPeriod;
                    lowLatencySlicePeriod = juce::jlimit(static_cast<double>(minPeriod),
                                                         static_cast<double>(maxPeriod), smoothedPeriod);
                }
                processChunk(data + i, 1, lowLatencySlicePeriod, ratio);
            }
        }
        else
        {
            smoothedPeriod = targetPeriod;
            for (int pos = 0; pos < numSamples; pos += kChunk)
                processChunk(data + pos, std::min(kChunk, numSamples - pos), targetPeriod, ratio);
        }

    }

    bool isNearUnity (float ratio) const
    {
        return std::abs(ratio - 1.0f) < 0.002f;
    }

private:
    static constexpr int kChunk = 256;
    static constexpr int kLowLatencyChunk = 32;

    static int nextPow2 (int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    double refinePitchMark (double predicted, double periodF) const
    {
        // A fixed-delay predictor can be forced to repeat part of a previous cycle.
        // Align those predicted cycles to a real waveform peak instead of assuming
        // that a period-only mark is phase-aligned. Restrict the search to available
        // history and a small fraction of a cycle so marks cannot jump octaves.
        const double radius = std::min(0.12 * periodF, 0.0015 * sr);
        const int64_t first = std::max<int64_t>(2, static_cast<int64_t>(std::ceil(predicted - radius)));
        const int64_t last = std::min<int64_t>(inCount - 3, static_cast<int64_t>(std::floor(predicted + radius)));
        if (first > last || predicted < 2.0 || predicted >= static_cast<double>(inCount - 3))
            return predicted;

        int64_t best = static_cast<int64_t>(std::llround(predicted));
        float bestMagnitude = 0.0f;
        // Prefer the same-polarity crest as the preceding predicted cycle. Searching
        // absolute magnitude alone can snap a mark onto a nearby opposite-polarity
        // harmonic peak, especially just after a falling pitch transition.
        const int64_t previous = static_cast<int64_t>(std::llround(predicted - periodF));
        const float reference = (previous >= 1 && previous < inCount)
            ? inRing[static_cast<size_t>(previous & inMask)] : 0.0f;
        const bool positiveReference = reference >= 0.0f;
        for (int64_t i = first; i <= last; ++i)
        {
            const float sample = inRing[static_cast<size_t>(i & inMask)];
            if ((sample >= 0.0f) != positiveReference)
                continue;
            const float value = std::abs(sample);
            if (value > bestMagnitude)
            {
                bestMagnitude = value;
                best = i;
            }
        }
        // Very quiet/near-zero reference points can have no candidate of the chosen
        // sign; fall back to the old bounded maximum rather than freezing the mark.
        if (bestMagnitude <= 1.0e-8f)
        {
            for (int64_t i = first; i <= last; ++i)
            {
                const float value = std::abs(inRing[static_cast<size_t>(i & inMask)]);
                if (value > bestMagnitude)
                {
                    bestMagnitude = value;
                    best = i;
                }
            }
        }

        // Sub-sample parabolic interpolation around the selected absolute peak.
        if (best > first && best < last)
        {
            const double ym = std::abs(inRing[static_cast<size_t>((best - 1) & inMask)]);
            const double y0 = std::abs(inRing[static_cast<size_t>( best      & inMask)]);
            const double yp = std::abs(inRing[static_cast<size_t>((best + 1) & inMask)]);
            const double denom = ym - 2.0 * y0 + yp;
            if (std::abs(denom) > 1.0e-12)
                return static_cast<double>(best) + juce::jlimit(-0.5, 0.5, 0.5 * (ym - yp) / denom);
        }
        return static_cast<double>(best);
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
        if (! lowLatencyExperiment)
            wo = std::max(std::min(wo, woMax), 0.5 * periodF);
        else
            wo = std::max(wo, 0.5 * periodF); // keep nominal cycle geometry; predict unavailable tail

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
            {
                markPos += periodF;
                if (lowLatencyExperiment)
                    markPos = refinePitchMark(markPos, periodF);
            }

            if (static_cast<int64_t>(std::floor(markPos + xNeed)) + 3 >= inCount)
                break;                                          // wait for more audio

            const double  limit  = static_cast<double>(inCount) - 3.0;   // newest position the cubic can use
            const int64_t grainStart = static_cast<int64_t>(std::ceil(nextTs - wo));
            const int64_t oFirst = std::max(grainStart, outRead);
            const int64_t oLast  = static_cast<int64_t>(std::floor(nextTs + wo));
            const bool startsLate = grainStart < outRead;

            for (int64_t o = oFirst; o <= oLast; ++o)
            {
                const float x = static_cast<float>(static_cast<double>(o) - nextTs + wo);
                if (x <= 0.0f)
                    continue;
                float w = 0.5f * (1.0f - std::cos(piOverW * x));      // Hann window
                if (startsLate)
                {
                    // Fixed-delay mode may discover a grain after its natural left edge
                    // has already passed. Adding its full mid-window weight at outRead
                    // creates clicks on sustained tones. Fade the newly arriving grain in
                    // causally over 32 samples; no past output is modified.
                    const float u = juce::jlimit(0.0f,
                        1.0f, static_cast<float>(o - outRead) / 32.0f);
                    const float ramp = u * u * (3.0f - 2.0f * u);
                    w *= ramp;
                }

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

                const size_t outputIndex = static_cast<size_t>(o & outMask);
                if (outRingEpoch[outputIndex] != ringEpoch)
                {
                    outRing[outputIndex] = 0.0f;
                    outRingEpoch[outputIndex] = ringEpoch;
                }
                outRing[outputIndex] += smp * w * gain;
            }

            nextTs += hop;
        }

        for (int i = 0; i < m; ++i)
        {
            const int64_t outputTime = outRead + i;
            const size_t idx = static_cast<size_t>(outputTime & outMask);
            const float shifted = outRingEpoch[idx] == ringEpoch ? outRing[idx] : 0.0f;
            if (outRingEpoch[idx] == ringEpoch)
                outRing[idx] = 0.0f;
            float result = shifted;
            if (lowLatencyExperiment && transitionBlendAge >= 0)
            {
                const int64_t dryTime = outputTime - latency;
                if (dryTime >= 0 && dryTime < inCount)
                {
                    const float dry = inRing[static_cast<size_t>(dryTime & inMask)];
                    const int attackSamples = std::max(1, static_cast<int>(std::ceil(sr * 0.0005)));
                    const float attack = juce::jlimit(0.0f, 1.0f,
                        static_cast<float>(transitionBlendAge) / static_cast<float>(attackSamples));
                    const float decay = juce::jlimit(0.0f, 1.0f,
                        1.0f - static_cast<float>(transitionBlendAge) / static_cast<float>(std::max(1, transitionBlendTotal)));
                    const float dryWeight = attack * decay;
                    result = shifted * (1.0f - dryWeight) + dry * dryWeight;
                }
                ++transitionBlendAge;
                if (transitionBlendAge >= transitionBlendTotal)
                    transitionBlendAge = -1;
            }
            data[i] = result;
        }
        outRead += m;
    }

    double sr = 44100.0;
    int maxPeriod = 800, minPeriod = 32, latency = 2000;
    int inSize = 0, outSize = 0;
    int64_t inMask = 0, outMask = 0;

    std::vector<float> inRing, outRing;
    std::vector<uint64_t> outRingEpoch;
    uint64_t ringEpoch = 0;

    int64_t inCount = 0;
    int64_t outRead = 0;
    double  nextTs  = 0.0;
    double  markPos = 0.0;
    double  lastPeriod = 220.0;
    double  smoothedPeriod = 220.0;
    double  lowLatencySlicePeriod = 220.0;
    double  lastInputPeriod = 220.0;
    int     transitionBlendAge = -1;
    int     transitionBlendTotal = 0;
    bool    formantPreserve = true;
    bool    lowLatencyExperiment = false;
    double  lowLatencyBudgetMs = 3.5;
};

} // namespace humtune
