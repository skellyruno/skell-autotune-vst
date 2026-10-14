#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace humtune
{

class PitchShifter
{
public:
    static constexpr double kMinHz = 70.0;       // lowest pitch ever supported (buffers are sized for it)

    // Two real-sample TD-PSOLA profiles. Low-latency mode shortens the available
    // output look-ahead; quality mode gives each grain a longer, safer overlap.
    // Neither mode fabricates missing samples. Lower fundamentals still need more delay.
    static constexpr double kLowLatencyPeriods = 1.15;
    static constexpr double kQualityLatencyPeriods = 1.70;
    static constexpr double kMaxHz = 1500.0;

    void prepare (double sampleRate, int /*blockSize*/)
    {
        sr = sampleRate;

        const int maxPeriodAlloc = std::max(32, static_cast<int>(std::ceil(sr / kMinHz)));
        // Reserve for quality mode even if the user starts in low-latency mode.
        const int latencyAlloc   = static_cast<int>(std::ceil(kQualityLatencyPeriods * maxPeriodAlloc)) + 8;

        inSize  = nextPow2(latencyAlloc + 4 * maxPeriodAlloc + 2 * kChunk);
        outSize = nextPow2(2 * latencyAlloc + 10 * maxPeriodAlloc + 4 * kChunk);
        inMask  = static_cast<int64_t>(inSize - 1);
        outMask = static_cast<int64_t>(outSize - 1);

        inRing.assign(static_cast<size_t>(inSize), 0.0f);
        outRing.assign(static_cast<size_t>(outSize), 0.0f);
        weightRing.assign(static_cast<size_t>(outSize), 0.0f);

        minPeriod = std::max(4, static_cast<int>(std::floor(sr / kMaxHz)));
        configure(kMinHz, lowLatencyMode);
    }

    void setMinFrequency (double hz) { configure(hz, lowLatencyMode); }

    // The switch selects a different delay/overlap profile without raising the
    // detector's minimum note. Reconfiguration happens only when the profile changes.
    void setLowLatencyMode (bool on) { configure(minFrequency, on); }

    void configure (double hz, bool lowLatency)
    {
        hz = juce::jlimit(kMinHz, 400.0, hz);
        const int nextMaxPeriod = std::max(32, static_cast<int>(std::ceil(sr / hz)));
        const double nextPeriods = lowLatency ? kLowLatencyPeriods : kQualityLatencyPeriods;
        const int nextLatency = static_cast<int>(std::ceil(nextPeriods * nextMaxPeriod)) + 8;
        const bool rangeChanged = nextMaxPeriod != maxPeriod || std::abs(hz - minFrequency) >= 0.5;
        const bool profileChanged = lowLatency != lowLatencyMode;
        const bool delayChanged = nextLatency != latency;
        const int previousLatency = latency;
        const bool historyExists = inCount > 0 && outRead > 0;
        minFrequency = hz;
        maxPeriod = nextMaxPeriod;
        latencyPeriods = nextPeriods;
        lowLatencyMode = lowLatency;
        latency = nextLatency;

        if ((rangeChanged || profileChanged || delayChanged) && historyExists)
        {
            // Preserve the streaming clocks and blend the old and new delay taps over
            // 5 ms. A direct tap jump at (for example) 130 Hz can land almost half a
            // cycle away and produce a click even though all DSP samples are valid.
            transitionFromLatency = previousLatency;
            transitionLength = std::max(32, static_cast<int>(std::lround(sr * 0.005)));
            transitionRemaining = transitionLength;
            lastPeriod = juce::jlimit(static_cast<double>(minPeriod),
                                      static_cast<double>(maxPeriod), lastPeriod);
            // Keep the current wet mix; its normal per-sample slew will move it
            // toward the new profile without dropping corrected signal abruptly.
        }
        else if (rangeChanged || profileChanged || delayChanged)
        {
            // Before streaming begins, a normal reset establishes clean timestamps.
            reset();
        }
    }

    void setFormantPreserve (bool on) { formantPreserve = on; }

    void reset()
    {
        std::fill(inRing.begin(), inRing.end(), 0.0f);
        std::fill(outRing.begin(), outRing.end(), 0.0f);
        std::fill(weightRing.begin(), weightRing.end(), 0.0f);
        inCount  = 0;
        outRead  = 0;
        nextTs   = 0.0;
        markPos  = -static_cast<double>(latency);
        lastPeriod = juce::jlimit(static_cast<double>(minPeriod),
                                  static_cast<double>(maxPeriod), sr / 200.0);
        smoothedRatio = 1.0;
        wetMixState = 0.0;
        transitionFromLatency = latency;
        transitionLength = 0;
        transitionRemaining = 0;
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
            // No reliable pitch (breath, consonants, silence): hold a sensible previous
            // period while the ratio returns to unity and the output fades to delayed dry.
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

    // Find the closest positive-going zero crossing near the predicted pitch mark.
    // PSOLA grains must be aligned to the waveform phase; a free-running mark clock
    // produces flanging/comb filtering even when the period estimate is correct.
    double alignToPitchMark (double predicted, double periodF, int64_t newest) const
    {
        if (predicted < 2.0 || newest < 4)
            return predicted;

        // Search less than half a period around the predicted *next* mark. Because the
        // mark clock advances by one estimated period, this can find the matching
        // positive-going crossing without accidentally choosing the adjacent cycle.
        const double radius = 0.45 * periodF;
        const int64_t first = std::max<int64_t>(1, static_cast<int64_t>(std::floor(predicted - radius)));
        const int64_t last  = std::min<int64_t>(newest - 1, static_cast<int64_t>(std::ceil(predicted + radius)));
        if (last <= first)
            return predicted;

        double best = predicted;
        double bestDistance = std::numeric_limits<double>::max();
        for (int64_t i = first; i <= last; ++i)
        {
            const float a = inRing[static_cast<size_t>((i - 1) & inMask)];
            const float b = inRing[static_cast<size_t>( i      & inMask)];
            if (a <= 0.0f && b > 0.0f)
            {
                const double frac = static_cast<double>(-a) /
                                    std::max(1.0e-12, static_cast<double>(b - a));
                const double crossing = static_cast<double>(i - 1) + juce::jlimit(0.0, 1.0, frac);
                const double distance = std::abs(crossing - predicted);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = crossing;
                }
            }
        }

        // If the local waveform does not provide a clean rising crossing, keep the
        // phase prediction. Switching between a zero crossing and a local peak makes
        // the source mark jump by a quarter cycle and is audible as roughness.
        return best;
    }

    void processChunk (float* data, int m, double periodF, double requestedRatio)
    {
        for (int i = 0; i < m; ++i)
            inRing[static_cast<size_t>((inCount + i) & inMask)] = data[i];
        inCount += m;

        // A tiny ratio smoother removes zippering when the detector updates its target
        // every few milliseconds. It is separate from the user-facing retune-speed control.
        const double ratioTime = 0.0025;
        const double ratioCoeff = std::exp(-static_cast<double>(m) / (sr * ratioTime));
        smoothedRatio = static_cast<double>(requestedRatio)
                      + (smoothedRatio - static_cast<double>(requestedRatio)) * ratioCoeff;
        const double ratio = juce::jlimit(0.6, 1.8, smoothedRatio);

        const double hop = periodF / ratio;
        const double margin = 12.0;

        // Use pitch-synchronous windows with a half-period source-anchor offset.
        // That keeps the grains phase-locked while moving the complete source window
        // farther into already-received audio. It is the key low-latency improvement:
        // no repeated/fabricated future cycles are used.
        double wo = std::max(0.56 * periodF, 0.58 * hop);
        const double rate = formantPreserve ? 1.0 : ratio;
        const double alignmentAllowance = 0.50 * periodF;
        // With the source centre half a period behind the mark, available look-ahead
        // is max(0, wo*rate - P/2). Bound both the ordinary and unpreserved-formant cases.
        const double safeLatency = static_cast<double>(latency) - margin;
        const double woMax = std::max(8.0, std::min(safeLatency - alignmentAllowance,
                                                  safeLatency / (1.0 + rate)));
        wo = std::max(8.0, std::min(wo, woMax));
        const double sourceHalf = wo * rate;
        const double sourceFuture = std::max(0.0, sourceHalf - 0.5 * periodF);
        const float piOverW = juce::MathConstants<float>::pi / static_cast<float>(wo);

        for (;;)
        {
            const double ta0 = nextTs - static_cast<double>(latency);

            // Keep a monotonic analysis-mark clock. Advance it in whole pitch periods, then
            // nudge each new mark onto the nearby waveform crossing/peak. Re-selecting a mark
            // independently for every synthesis grain causes phase jitter as the target ratio
            // changes; stateful pitch marks are much more stable for sustained vowels.
            while (markPos + 0.5 * periodF < ta0)
            {
                markPos += periodF;
                if (markPos >= 2.0 && markPos < static_cast<double>(inCount - 3))
                    markPos = alignToPitchMark(markPos, periodF, inCount - 3);
            }

            // Do not synthesize a grain until its complete source window exists. The old
            // implementation wrapped unavailable future samples back one period; that
            // periodic guess was the main source of metallic/chipmunk artifacts at low delay.
            if (markPos < 0.5 * periodF + sourceHalf + 2.0)
            {
                // Stream start: skip incomplete grains rather than building them from zeros.
                nextTs += hop;
                continue;
            }
            // Only wait for the small right-hand tail that is genuinely needed by the
            // interpolation kernel. The rest of the grain comes from available history.
            if (markPos + sourceFuture + 3.0 >= static_cast<double>(inCount))
                break;
            const double mark = markPos;

            const int64_t oFirst = static_cast<int64_t>(std::ceil (nextTs - wo));
            const int64_t oLast  = static_cast<int64_t>(std::floor(nextTs + wo));

            for (int64_t o = std::max(oFirst, outRead); o <= oLast; ++o)
            {
                const double xd = static_cast<double>(o) - nextTs + wo;
                if (xd <= 0.0 || xd >= 2.0 * wo)
                    continue;
                const float x = static_cast<float>(xd);
                const float w = 0.5f * (1.0f - std::cos(piOverW * x));

                const double pos = mark - 0.5 * periodF
                                 + (static_cast<double>(o) - nextTs) * rate;
                const double fl = std::floor(pos);
                const int64_t a = static_cast<int64_t>(fl);
                if (a < 1 || a + 2 >= inCount)
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

                const size_t outIdx = static_cast<size_t>(o & outMask);
                outRing[outIdx] += smp * w;
                weightRing[outIdx] += w;
            }

            nextTs += hop;
        }

        // Crossfade to the exact delayed dry path as correction approaches unity. Switching
        // directly from a granular waveform to dry audio on a voiced/unvoiced transition
        // can create a click even though the reported sample latency is unchanged.
        const double ratioDelta = std::abs(smoothedRatio - 1.0);
        const double targetWetMix = juce::jlimit(0.0, 1.0,
                                               (ratioDelta - 0.0002) / 0.0018);
        const double mixStep = 1.0 / std::max(1.0, sr * 0.003); // 3 ms linear fade
        for (int i = 0; i < m; ++i)
        {
            const int64_t outputIndex = outRead + i;
            const size_t idx = static_cast<size_t>(outputIndex & outMask);
            const int64_t sourceIndex = outputIndex - latency;
            const float dryNew = sourceIndex >= 0
                               ? inRing[static_cast<size_t>(sourceIndex & inMask)]
                               : 0.0f;
            float dry = dryNew;
            if (transitionRemaining > 0 && transitionLength > 0)
            {
                const int64_t oldSourceIndex = outputIndex - transitionFromLatency;
                const float dryOld = oldSourceIndex >= 0
                                   ? inRing[static_cast<size_t>(oldSourceIndex & inMask)]
                                   : 0.0f;
                const float alpha = 1.0f - static_cast<float>(transitionRemaining)
                                          / static_cast<float>(transitionLength);
                dry = dryOld + (dryNew - dryOld) * alpha;
                --transitionRemaining;
            }

            const float weight = weightRing[idx];
            const double desiredMix = weight > 1.0e-4f ? targetWetMix : 0.0;
            // Slew the mix per sample (not per host block) and only fade in when a complete
            // wet grain exists. That avoids both note-change clicks and startup grain pops.
            if (wetMixState < desiredMix)
                wetMixState = std::min(desiredMix, wetMixState + mixStep);
            else if (wetMixState > desiredMix)
                wetMixState = std::max(desiredMix, wetMixState - mixStep);

            if (weight > 1.0e-4f && wetMixState > 0.0)
            {
                // Normalize by the actual local overlap, rather than an average hop/width
                // gain. Then fade to the delayed input for micro-corrections/unvoiced audio.
                const float wet = outRing[idx] / weight;
                const float wetMix = static_cast<float>(wetMixState);
                data[i] = dry + wetMix * (wet - dry);
            }
            else
            {
                data[i] = dry;
            }

            outRing[idx] = 0.0f;
            weightRing[idx] = 0.0f;
        }
        outRead += m;
    }

    double sr = 44100.0;
    double minFrequency = kMinHz;
    double latencyPeriods = kLowLatencyPeriods;
    bool lowLatencyMode = true;
    int maxPeriod = 800, minPeriod = 32, latency = 2000;
    int inSize = 0, outSize = 0;
    int64_t inMask = 0, outMask = 0;

    std::vector<float> inRing, outRing, weightRing;

    int64_t inCount = 0;
    int64_t outRead = 0;
    double  nextTs  = 0.0;
    double  markPos = 0.0;
    double  lastPeriod = 220.0;
    double  smoothedRatio = 1.0;
    double  wetMixState = 0.0;
    int     transitionFromLatency = 0;
    int     transitionLength = 0;
    int     transitionRemaining = 0;
    bool    formantPreserve = true;
};

} // namespace humtune
