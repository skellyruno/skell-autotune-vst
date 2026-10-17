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

    // Standard mode has more source history for resynthesis; live mode retains
    // its fixed budget. Both use the same causal overlap engine and dry path.
    static constexpr double kLatencyPeriods = 1.3;
    static constexpr double kMaxHz = 1500.0;

    void prepare (double sampleRate, int /*blockSize*/)
    {
        sr = sampleRate;

        const int maxPeriodAlloc = std::max(32, static_cast<int>(std::ceil(sr / kMinHz)));
        const int latencyAlloc   = static_cast<int>(std::ceil(kLatencyPeriods * maxPeriodAlloc)) + 8;

        inSize  = nextPow2(latencyAlloc + 4 * maxPeriodAlloc + 2 * kChunk);
        inMask  = static_cast<int64_t>(inSize - 1);

        inRing.assign(static_cast<size_t>(inSize), 0.0f);

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
        // Invalidate input history with the timeline; no ring clears or allocations
        // are needed when a host changes mode/range on its audio thread.
        inCount  = 0;
        outRead  = 0;
        nextTs   = 0.0;
        markPos  = -static_cast<double>(latency);
        lastPeriod = juce::jlimit(static_cast<double>(minPeriod),
                                  static_cast<double>(maxPeriod), sr / 200.0);
        smoothedPeriod = lastPeriod;
        slicePeriod = lastPeriod;
        for (auto& grain : liveGrains) grain.active = false;
        wetMix = 0.0f;
        neutralSamples = 0;
        lastShiftRatio = 1.0;
        smoothedFormantShiftRatio = formantShiftRatioTarget;
    }

    int getLatencySamples() const { return latency; }

    void process (float* data, int numSamples, float periodSamples, float shiftRatio)
    {
        if (numSamples <= 0 || inRing.empty())
            return;

        voiced = std::isfinite(periodSamples) && periodSamples >= static_cast<float>(minPeriod);
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
        lastShiftRatio = ratio;

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
                    slicePeriod = juce::jlimit(static_cast<double>(minPeriod),
                                                         static_cast<double>(maxPeriod), smoothedPeriod);
                    smoothedFormantShiftRatio = formantShiftRatioTarget
                        + (smoothedFormantShiftRatio - formantShiftRatioTarget) * formantCoeff;
                    snapNeutralFormantShiftIfSettled();
                }
                processSample(data[i], slicePeriod, ratio, smoothedFormantShiftRatio);
            }
        }
    }


    // Keep the same delay/history while tuning is bypassed. Re-enabling does
    // not need to refill a cold ring or insert a new latency-length silence gap.
    void processBypassed(float* data, int numSamples)
    {
        if (inRing.empty()) return;
        voiced = false;
        for (int i = 0; i < numSamples; ++i)
        {
            if (wetMix > 0.0f)
            {
                // Keep the outgoing grain phase until the normal 3 ms wet ramp
                // reaches dry. A direct phase switch clicks on corrected vocals.
                processSample(data[i], slicePeriod, lastShiftRatio, smoothedFormantShiftRatio);
            }
            else
            {
                inRing[static_cast<size_t>(inCount & inMask)] = data[i];
                const int64_t dryTime = outRead - latency;
                data[i] = dryTime >= 0 ? inRing[static_cast<size_t>(dryTime & inMask)] : 0.0f;
                ++inCount;
                ++outRead;
            }
        }
        if (wetMix <= 0.0f)
        {
            for (auto& grain : liveGrains) grain.active = false;
            neutralSamples = 0;
            nextTs = static_cast<double>(outRead);
            markPos = nextTs - latency;
        }
    }

    bool isNearUnity (float ratio) const
    {
        return std::abs(ratio - 1.0f) < 0.002f;
    }

private:
    static constexpr int kChunk = 256;
    static constexpr int kLowLatencyChunk = 32;
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
        double fadeStart = -1.0;
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

    void processSample(float& sample, double period, double ratio, double formantShiftRatio)
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
                    grain = {nextTs, markPos, halfWindow, period, period / halfWindow,
                             nextTs - halfWindow < time - 1.0 ? time : -1.0, true};
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
            double w = 0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * offset / grain.halfWindow));
            if (grain.fadeStart >= 0.0)
            {
                // A changing period can move an upcoming window's left edge into
                // the past. Introduce that window smoothly rather than inserting
                // a nonzero overlap weight at the current sample.
                const double u = juce::jlimit(0.0, 1.0, (time - grain.fadeStart) / std::max(1.0, sr * 0.001));
                w *= u * u * (3.0 - 2.0 * u);
            }
            total += w * readPredictedInput(grain.source + offset * grain.rate, grain.period);
            weight += w;
        }
        const int64_t dryTime = outRead - latency;
        const float dry = dryTime >= 0 ? inRing[static_cast<size_t>(dryTime & inMask)] : 0.0f;
        const float shifted = weight > 1.0e-6 ? static_cast<float>(total / weight) : dry;
        const bool needsShift = std::abs(ratio - 1.0) > 0.00001 || formantShiftIsActive();
        neutralSamples = needsShift ? 0 : std::min(neutralSamples + 1, static_cast<int>(sr));
        // Keep the resynthesis phase continuous through short unity crossings.
        // Switching to dry at every vibrato zero crossing combines unrelated phases.
        const bool holdPhase = wetMix > 0.0f && neutralSamples < static_cast<int>(sr * 0.08);
        const float targetWet = voiced && (needsShift || holdPhase) ? 1.0f : 0.0f;
        const float step = static_cast<float>(1.0 / std::max(1.0, sr * 0.003));
        wetMix += juce::jlimit(-step, step, targetWet - wetMix);
        sample = dry + wetMix * (shifted - dry);
        ++outRead;
    }

    std::array<LiveGrain, 16> liveGrains {};
    double sr = 44100.0;
    int maxPeriod = 800, minPeriod = 32, latency = 2000;
    int inSize = 0;
    int64_t inMask = 0;

    std::vector<float> inRing;
    int neutralSamples = 0;
    float wetMix = 0.0f;
    bool voiced = false;

    int64_t inCount = 0;
    int64_t outRead = 0;
    double  nextTs  = 0.0;
    double  markPos = 0.0;
    double  lastPeriod = 220.0;
    double  smoothedPeriod = 220.0;
    double  slicePeriod = 220.0;
    double  lastShiftRatio = 1.0;
    bool    formantPreserve = true;
    bool    lowLatencyExperiment = false;
    double  lowLatencyBudgetMs = 3.5;
    double  formantShiftRatioTarget = 1.0;
    double  smoothedFormantShiftRatio = 1.0;
};

} // namespace humtune
