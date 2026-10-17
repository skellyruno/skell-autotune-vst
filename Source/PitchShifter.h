#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
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
    // Lowered from 1.6 to 1.3 (about 19% less delay: 80 Hz range 20.2 -> 16.4 ms at 48 kHz).
    // A floor-edge stress sweep (tests/latency_sweep.cpp: 80/130/180 Hz floors, shifts of
    // -2/0/+1 semitones, with and without vibrato) showed identical pitch accuracy, the same
    // worst-case level deviation as 1.6 and fewer waveform discontinuities. Below ~1.25 the
    // output level starts to drift upward (1.65 dB at 1.2, 2.8 dB at 1.1), so do not go lower
    // without a real-vocal comparison. The experimental mode uses its own budget.
    static constexpr double kLatencyPeriods = 1.3;
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

    // Independent vocal-tract/formant colour shift. Zero keeps the current
    // behaviour exactly; non-zero values move the spectral envelope without
    // changing the grain spacing that determines the corrected fundamental.
    // The range is deliberately modest because extreme rates can make PSOLA
    // grains too short for a stable overlap at low fundamentals.
    void setFormantShiftSemitones (float semitones) noexcept
    {
        if (! std::isfinite(semitones))
            semitones = 0.0f;
        formantShiftRatioTarget = std::exp2(static_cast<double>(juce::jlimit(-4.0f, 4.0f, semitones)) / 12.0);
    }

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
        for (auto& grain : liveGrains) grain.active = false;
        lowLatencyWet = 0.0f;
        smoothedFormantShiftRatio = formantShiftRatioTarget;
    }

    int getLatencySamples() const { return latency; }

    void process (float* data, int numSamples, float periodSamples, float shiftRatio)
    {
        if (numSamples <= 0 || inRing.empty())
            return;

        lowLatencyVoiced = std::isfinite(periodSamples) && periodSamples >= static_cast<float>(minPeriod);
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
            const double formantCoeff = std::exp(-static_cast<double>(kLowLatencyChunk)
                                                  / (sr * kFormantShiftSmoothingSeconds));
            for (int i = 0; i < numSamples; ++i)
            {
                if ((inCount % kLowLatencyChunk) == 0)
                {
                    smoothedPeriod = smoothingCoeff * smoothedPeriod
                                   + (1.0 - smoothingCoeff) * targetPeriod;
                    lowLatencySlicePeriod = juce::jlimit(static_cast<double>(minPeriod),
                                                         static_cast<double>(maxPeriod), smoothedPeriod);
                    smoothedFormantShiftRatio = formantShiftRatioTarget
                        + (smoothedFormantShiftRatio - formantShiftRatioTarget) * formantCoeff;
                    snapNeutralFormantShiftIfSettled();
                }
                processLowLatencySample(data[i], lowLatencySlicePeriod, ratio, smoothedFormantShiftRatio);
            }
        }
        else if (formantShiftIsActive())
        {
            // Use a persistent 32-sample timeline for the new control. Automation
            // and host block boundaries therefore cannot change its smoothing rate.
            // The neutral setting stays on the old 256-sample path below to keep
            // existing presets/renders bit-identical.
            const double formantCoeff = std::exp(-static_cast<double>(kFormantUpdateChunk)
                                                  / (sr * kFormantShiftSmoothingSeconds));
            int pos = 0;
            while (pos < numSamples)
            {
                const int offset = static_cast<int>(inCount % kFormantUpdateChunk);
                const int n = std::min(numSamples - pos, kFormantUpdateChunk - offset);
                if (offset == 0)
                {
                    smoothedFormantShiftRatio = formantShiftRatioTarget
                        + (smoothedFormantShiftRatio - formantShiftRatioTarget) * formantCoeff;
                    snapNeutralFormantShiftIfSettled();
                }
                processChunk(data + pos, n, targetPeriod, ratio, smoothedFormantShiftRatio);
                pos += n;
            }
        }
        else
        {
            smoothedPeriod = targetPeriod;
            for (int pos = 0; pos < numSamples; pos += kChunk)
                processChunk(data + pos, std::min(kChunk, numSamples - pos), targetPeriod, ratio,
                             smoothedFormantShiftRatio);
        }

    }

    bool isNearUnity (float ratio) const
    {
        return std::abs(ratio - 1.0f) < 0.002f;
    }

private:
    static constexpr int kChunk = 256;
    static constexpr int kLowLatencyChunk = 32;
    static constexpr int kFormantUpdateChunk = 32;
    static constexpr double kFormantShiftSmoothingSeconds = 0.012;

    bool formantShiftIsActive() const noexcept
    {
        return std::abs(formantShiftRatioTarget - 1.0) > 1.0e-7
            || std::abs(smoothedFormantShiftRatio - 1.0) > 1.0e-7;
    }

    void snapNeutralFormantShiftIfSettled() noexcept
    {
        if (std::abs(formantShiftRatioTarget - 1.0) < 1.0e-7
            && std::abs(smoothedFormantShiftRatio - 1.0) < 1.0e-6)
            smoothedFormantShiftRatio = 1.0;
    }

    static int nextPow2 (int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    struct LiveGrain
    {
        double centre = 0.0, source = 0.0, halfWindow = 1.0, period = 1.0, rate = 1.0;
        bool active = false;
    };

    float readInput(double pos) const noexcept
    {
        const double fl = std::floor(pos);
        const auto a = static_cast<int64_t>(fl);
        if (a < 1 || a - 1 < inCount - inSize || a + 2 >= inCount)
            return 0.0f;
        const float t = static_cast<float>(pos - fl);
        const float p0 = inRing[static_cast<size_t>((a - 1) & inMask)];
        const float p1 = inRing[static_cast<size_t>(a & inMask)];
        const float p2 = inRing[static_cast<size_t>((a + 1) & inMask)];
        const float p3 = inRing[static_cast<size_t>((a + 2) & inMask)];
        return p1 + 0.5f * t * (p2 - p0 + t * (2*p0 - 5*p1 + 4*p2 - p3 + t * (3*(p1-p2) + p3-p0)));
    }

    float readPredictedInput(double pos, double period) const noexcept
    {
        const double limit = static_cast<double>(inCount - 3);
        if (pos > limit)
            pos -= std::ceil((pos - limit) / period) * period;
        // At the edge of available history, crossfade to the preceding cycle
        // BEFORE wrapping. A discontinuous cycle substitution crackles on a
        // changing or imperfectly periodic voice even if a sine-wave test passes.
        const double fade = std::max(1.0, std::min(period * 0.08, latency * 0.5));
        const double u = juce::jlimit(0.0, 1.0, (pos - (limit - fade)) / fade);
        const float blend = static_cast<float>(u * u * (3.0 - 2.0 * u));
        return readInput(pos) * (1.0f - blend) + readInput(pos - period) * blend;
    }

    void processLowLatencySample(float& sample, double period, double ratio, double formantShiftRatio)
    {
        inRing[static_cast<size_t>(inCount & inMask)] = sample;
        ++inCount;
        const double time = static_cast<double>(outRead);
        const double rate = (formantPreserve ? 1.0 : ratio) * formantShiftRatio;
        const double halfWindow = std::max(period / rate, 0.5 * period);
        const double hop = period / ratio;
        // Start each grain at its LEFT edge. The former fixed-budget path waited
        // for the source centre, then inserted the middle of a window into output
        // already in flight. That dropped overlap weight and modulated the level.
        while (nextTs - halfWindow <= time)
        {
            const double sourceTime = nextTs - latency;
            while (markPos + 0.5 * period < sourceTime)
                markPos += period;
            for (auto& grain : liveGrains)
            {
                if (!grain.active)
                {
                    grain = {nextTs, markPos, halfWindow, period, period / halfWindow, true};
                    break;
                }
            }
            nextTs += hop;
        }
        double total = 0.0, weight = 0.0;
        for (auto& grain : liveGrains)
        {
            if (!grain.active) continue;
            const double offset = time - grain.centre;
            if (offset >= grain.halfWindow) { grain.active = false; continue; }
            if (offset < -grain.halfWindow) continue;
            const double w = 0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * offset / grain.halfWindow));
            total += w * readPredictedInput(grain.source + offset * grain.rate, grain.period);
            weight += w;
        }
        const int64_t dryTime = outRead - latency;
        const float dry = dryTime >= 0 ? inRing[static_cast<size_t>(dryTime & inMask)] : 0.0f;
        const float shifted = weight > 1.0e-6 ? static_cast<float>(total / weight) : dry;
        const bool needsShift = std::abs(ratio - 1.0) > 0.00001 || formantShiftIsActive();
        const float targetWet = lowLatencyVoiced && needsShift ? 1.0f : 0.0f;
        const float step = static_cast<float>(1.0 / std::max(1.0, sr * 0.003));
        lowLatencyWet += juce::jlimit(-step, step, targetWet - lowLatencyWet);
        sample = dry + lowLatencyWet * (shifted - dry);
        ++outRead;
    }

    void processChunk (float* data, int m, double periodF, double ratio, double formantShiftRatio)
    {
        for (int i = 0; i < m; ++i)
            inRing[static_cast<size_t>((inCount + i) & inMask)] = data[i];
        inCount += m;

        // Output spacing of the grains (this sets the new pitch)
        const double hop    = periodF / ratio;
        const double margin = 8.0;

        // The output grain spacing controls corrected pitch; source read rate controls
        // vocal colour. Keeping these rates independent preserves formants during tuning
        // while allowing a separate, modest timbre shift when requested.
        const double formantRate = (formantPreserve ? 1.0 : ratio) * formantShiftRatio;
        double wo = periodF / formantRate;

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
            {
                markPos += periodF;
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
            data[i] = result;
        }
        outRead += m;
    }

    std::array<LiveGrain, 16> liveGrains {};
    double sr = 44100.0;
    int maxPeriod = 800, minPeriod = 32, latency = 2000;
    int inSize = 0, outSize = 0;
    int64_t inMask = 0, outMask = 0;

    std::vector<float> inRing, outRing;
    float lowLatencyWet = 0.0f;
    bool lowLatencyVoiced = false;
    std::vector<uint64_t> outRingEpoch;
    uint64_t ringEpoch = 0;

    int64_t inCount = 0;
    int64_t outRead = 0;
    double  nextTs  = 0.0;
    double  markPos = 0.0;
    double  lastPeriod = 220.0;
    double  smoothedPeriod = 220.0;
    double  lowLatencySlicePeriod = 220.0;
    bool    formantPreserve = true;
    bool    lowLatencyExperiment = false;
    double  lowLatencyBudgetMs = 3.5;
    double  formantShiftRatioTarget = 1.0;
    double  smoothedFormantShiftRatio = 1.0;
};

} // namespace humtune
