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
        dryReleaseCoeff = static_cast<float>(std::exp(-1.0 / (std::max(1.0, sr) * 0.0025)));
        dryAttackCoeff = static_cast<float>(std::exp(-1.0 / (std::max(1.0, sr) * 0.004)));
        crossfadeEnergyCoeff = static_cast<float>(std::exp(-1.0 / (std::max(1.0, sr) * 0.0015)));
        crossfadeGainCoeff = static_cast<float>(std::exp(-1.0 / (std::max(1.0, sr) * 0.0012)));

        const int maxPeriodAlloc = std::max(32, static_cast<int>(std::ceil(sr / kMinHz)));
        const int latencyAlloc   = static_cast<int>(std::ceil(kLatencyPeriods * maxPeriodAlloc)) + 8;

        inSize  = nextPow2(latencyAlloc + 4 * maxPeriodAlloc + 2 * kChunk);
        outSize = nextPow2(2 * latencyAlloc + 10 * maxPeriodAlloc + 4 * kChunk);
        inMask  = static_cast<int64_t>(inSize - 1);
        outMask = static_cast<int64_t>(outSize - 1);

        inRing.assign(static_cast<size_t>(inSize), 0.0f);
        outRing.assign(static_cast<size_t>(outSize), 0.0f);
        outWeight.assign(static_cast<size_t>(outSize), 0.0f);
        outAbsContribution.assign(static_cast<size_t>(outSize), 0.0f);
        outContributors.assign(static_cast<size_t>(outSize), 0);
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

    void setFormantPreserve (bool on) noexcept { formantPreserve = on; }

    // A voiced vowel uses pitch-synchronous resynthesis. On consonants, breaths and
    // silence, blend toward an exactly latency-aligned dry signal instead of
    // imposing a guessed voiced cycle on aperiodic audio. This path adds no delay,
    // makes no allocation, and keeps processing the wet timeline continuously.
    void setVoiced (bool voiced) noexcept { setDryPassthrough(!voiced); }
    void setPhaseLockedSource(bool on) noexcept { phaseLockEnabled=on; }
    // Opt-in expensive trace instrumentation. Disabled in native DAW processing.
    void setDetailedDiagnosticsEnabled(bool enabled) noexcept { detailedDiagnostics = enabled; }
    // Internal A/B gate for deterministic regression testing. Production defaults
    // to the bounded, level-aware handoff in experimental Low Latency mode.
    void setEnergyMatchedHandoffEnabled(bool enabled) noexcept { energyMatchedHandoff = enabled; }
    void setDryPassthrough (bool bypassWet) noexcept
    {
        dryMixTarget = bypassWet ? 1.0f : 0.0f;
    }
    float getDryMixForDiagnostics() const noexcept { return dryMix; }
    struct GrainDiagnostics {
        float coverage = 0, appliedGain = 1, markDelta = 0, maxOutputStep = 0, phaseMatchScore = 0;
        float overlapMatchScore = 0, overlapShiftSamples = 0;
        float cancellationFraction = 0; // offline-only: destructive grain overlap
        int contributors = 0;
    };
    // Valid on the DSP thread only: the offline trace samples this per analysis hop.
    GrainDiagnostics getGrainDiagnostics() const noexcept { return grainDiagnostics; }



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
        previousPhaseAnchor = -1.0e12;
        previousPhaseOffset = 0.0;
        phaseScore = 0.0f;
        lastRefinedMark = -1.0e12;
        lastPeriod = juce::jlimit(static_cast<double>(minPeriod),
                                  static_cast<double>(maxPeriod), sr / 200.0);
        smoothedPeriod = lastPeriod;
        lowLatencySlicePeriod = lastPeriod;
        smoothedFormantShiftRatio = formantShiftRatioTarget;
        smoothedFormantPreserve = formantPreserve ? 1.0 : 0.0;
        dryMix = 0.0f;
        dryMixTarget = 0.0f;
        crossfadeWetPower = crossfadeDryPower = crossfadeCov = 0.0f;
        crossfadeGain = 1.0f;
        grainDiagnostics = {};
        lastOutput = 0.0f;
    }

    int getLatencySamples() const { return latency; }

    void process (float* data, int numSamples, float periodSamples, float shiftRatio)
    {
        if (numSamples <= 0 || inRing.empty())
            return;

        diagnosticCoverageSum = diagnosticGainSum = 0.0f;
        diagnosticCancellationSum = 0.0f;
        diagnosticCancellationCount = 0;
        grainDiagnostics.cancellationFraction = 0.0f;
        diagnosticContributorsSum = 0;
        diagnosticCount = 0;
        grainDiagnostics.maxOutputStep = 0.0f;
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
                    smoothFormantPreserve(kLowLatencyChunk);
                }
                processChunk(data + i, 1, lowLatencySlicePeriod, ratio, smoothedFormantShiftRatio);
            }
        }
        else if (formantShiftIsActive() || formantPreserveIsTransitioning())
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
                    smoothFormantPreserve(kFormantUpdateChunk);
                }
                for (int k = 0; k < n; ++k)
                    processChunk(data + pos + k, 1, targetPeriod, ratio, smoothedFormantShiftRatio);
                pos += n;
            }
        }
        else
        {
            smoothedPeriod = targetPeriod;
            // Causal one-sample Standard scheduling: grains must never depend
            // on how much future input a host callback delivered in a batch.
            for (int pos = 0; pos < numSamples; ++pos)
                processChunk(data + pos, 1, targetPeriod, ratio, smoothedFormantShiftRatio);
        }
        if (detailedDiagnostics && diagnosticCancellationCount > 0)
            grainDiagnostics.cancellationFraction = diagnosticCancellationSum
                / static_cast<float>(diagnosticCancellationCount);
        if (lowLatencyExperiment && diagnosticCount > 0)
        {
            const float inverse = 1.0f / static_cast<float>(diagnosticCount);
            grainDiagnostics.coverage = diagnosticCoverageSum * inverse;
            grainDiagnostics.appliedGain = diagnosticGainSum * inverse;
            grainDiagnostics.contributors = static_cast<int>(std::lround(
                diagnosticContributorsSum * inverse));
        }

    }

    bool isNearUnity (float ratio) const
    {
        return std::abs(ratio - 1.0f) < 0.002f;
    }

private:
    static constexpr int kChunk = 256;
    static constexpr int kLowLatencyChunk = 32;
    static constexpr double kPredictivePhaseOffsetSeconds = 0.00075;
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

    bool formantPreserveIsTransitioning() const noexcept
    {
        return std::abs(smoothedFormantPreserve - (formantPreserve ? 1.0 : 0.0)) > 1.0e-7;
    }

    void smoothFormantPreserve(int samples) noexcept
    {
        const double target = formantPreserve ? 1.0 : 0.0;
        if (smoothedFormantPreserve == target)
            return;
        const double coeff = std::exp(-static_cast<double>(samples)
                                      / (std::max(1.0, sr) * kFormantShiftSmoothingSeconds));
        smoothedFormantPreserve = target + (smoothedFormantPreserve - target) * coeff;
        if (std::abs(smoothedFormantPreserve - target) < 1.0e-6)
            smoothedFormantPreserve = target;
    }

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
        // Limit phase correction relative to the last accepted mark. Even if a
        // vowel's strongest harmonic switches polarity/crest, adjacent grains
        // should not snap to different waveform lobes.
        if (lastRefinedMark > -1.0e11)
            predicted = std::clamp(predicted,
                                   lastRefinedMark + 0.88 * periodF,
                                   lastRefinedMark + 1.12 * periodF);
        const double radius = std::min(0.07 * periodF, 0.0010 * sr);
        const int64_t first = std::max<int64_t>(2, static_cast<int64_t>(std::ceil(predicted - radius)));
        const int64_t last = std::min<int64_t>(inCount - 3, static_cast<int64_t>(std::floor(predicted + radius)));
        if (first > last || predicted < 2.0 || predicted >= static_cast<double>(inCount - 3))
            return predicted;

        int64_t best = static_cast<int64_t>(std::llround(predicted));
        float bestMagnitude = 0.0f;
        // Prefer the same-polarity crest as the preceding predicted cycle. Searching
        // absolute magnitude alone can snap a mark onto a nearby opposite-polarity
        // harmonic peak, especially just after a falling pitch transition.
        const int64_t previous = static_cast<int64_t>(std::llround(
            lastRefinedMark > 1.0 ? lastRefinedMark : predicted - periodF));
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

    // Phase-lock the read anchor to the previous, *recorded* pitch cycle.
    // No future input is accessed. Keep the output grain clock unchanged and
    // adjust only the grain's source phase by a small continuous displacement.
    double phaseMatchOffset (double predicted, double period) noexcept
    {
        if (!phaseLockEnabled || inCount < static_cast<int64_t>(3.0 * period) + 32)
            return 0.0;
        // The 1.9 ms predictor needs a longer recorded-history correlation
        // than Standard. 36 samples proved more resistant to wrong harmonic
        // lobe picks on the difficult-note regions of the supplied test vocal.
        // Standard remains at its v31 window and phase slew rate.
        const int window = lowLatencyExperiment ? 36 : 24;
        const double newest = static_cast<double>(inCount - 4 - window);
        double anchor = predicted;
        if (anchor > newest)
            anchor -= std::ceil((anchor-newest)/period)*period;
        if (anchor < period + window + 4.0)
            return 0.0;
        if (previousPhaseAnchor < 0.0)
        {
            previousPhaseAnchor = anchor;
            return 0.0;
        }
        // Move the reference into the recorded history and choose the cycle
        // equivalent nearest to this grain's historical analysis mark.
        double reference = previousPhaseAnchor;
        if (reference > newest)
            reference -= std::ceil((reference-newest)/period)*period;
        while (reference + period < anchor - 0.48*period)
            reference += period;
        while (reference - period > anchor + 0.48*period)
            reference -= period;
        if (reference < window + 4.0 || reference >= newest)
        {
            previousPhaseAnchor = anchor;
            return 0.0;
        }
        const int base = static_cast<int>(std::lround(anchor));
        const int ref = static_cast<int>(std::lround(reference));
        const int maxOffset = std::clamp(static_cast<int>(std::lround(0.045*period)), 2, 14);
        double bestScore = -2.0;
        int bestShift = 0;
        // Every correlation sample must be from recorded history. The left-edge
        // scheduler can request an anchor close to inCount: a positive search
        // displacement would otherwise read unwritten ring slots. Those slots
        // may contain stale samples from an earlier wrap or reset.
        const int64_t newestValid = inCount - 3; // same safety margin as cubic reads
        const int64_t oldestValid = std::max<int64_t>(0, inCount - inSize);
        if (static_cast<int64_t>(ref) - window < oldestValid ||
            static_cast<int64_t>(ref) + window > newestValid)
            return 0.0;
        for(int shift=-maxOffset;shift<=maxOffset;++shift)
        {
            const int64_t lo = static_cast<int64_t>(base) + shift - window;
            const int64_t hi = static_cast<int64_t>(base) + shift + window;
            if (lo < oldestValid || hi > newestValid)
                continue;
            double xy=0, xx=0, yy=0;
            for(int j=-window;j<=window;j+=2)
            {
                const double a=inRing[static_cast<size_t>((base+shift+j)&inMask)];
                const double b=inRing[static_cast<size_t>((ref+j)&inMask)];
                xy+=a*b;xx+=a*a;yy+=b*b;
            }
            const double score=(xx>1e-8&&yy>1e-8)?xy/std::sqrt(xx*yy):-1;
            if(score>bestScore){bestScore=score;bestShift=shift;}
        }
        phaseScore=static_cast<float>(std::max(0.0, bestScore));
        grainDiagnostics.phaseMatchScore=phaseScore;
        // A wrong or weak match must not jump the phase of a grain.
        if(bestScore < 0.87 || std::abs(bestShift) > 0.045*period)
        {
            previousPhaseAnchor=anchor;
            previousPhaseOffset=0;
            return 0.0;
        }
        // Small, persistent adjustments; do not steal the hop's musical pitch.
        const double maxPhaseStep = (lowLatencyExperiment ? 0.020 : 0.015) * period;
        const double correction=std::clamp(static_cast<double>(bestShift),
                                            previousPhaseOffset-maxPhaseStep,
                                            previousPhaseOffset+maxPhaseStep);
        previousPhaseOffset=correction;
        previousPhaseAnchor=anchor+correction;
        return correction;
    }

    double overlapPhaseOffset (double ts, double mark, double period, double rate,
                               double wo, int64_t oFirst, int64_t oLast,
                               double latest) noexcept
    {
        // Compare each new predictive grain to audio already scheduled in the output
        // overlap. An 8%-of-cycle bound handles mild pitch-period drift without
        // searching unrelated half-cycles. This is used ONLY for Low Latency ON.
        const int shiftRadius = std::clamp(static_cast<int>(0.080 * period), 1, 36);
        const int64_t start = std::max<int64_t>(oFirst, outRead);
        const int64_t stop = std::min<int64_t>(oLast, start + static_cast<int64_t>(period));
        if (stop - start < 24) return 0.0;
        double bestScore = -2.0, centerScore = -2.0;
        int bestShift = 0;
        for (int offset = -shiftRadius; offset <= shiftRadius; ++offset)
        {
            double cross = 0, aa = 0, bb = 0;
            int taps = 0;
            for (int64_t o = start; o <= stop; o += 4)
            {
                const size_t idx = static_cast<size_t>(o & outMask);
                if (outRingEpoch[idx] != ringEpoch || outWeight[idx] < 0.25f)
                    continue;
                double pos = mark + offset + (static_cast<double>(o) - ts)*rate
                             - kPredictivePhaseOffsetSeconds*sr;
                if (pos > latest) pos -= std::ceil((pos-latest)/period)*period;
                const int64_t i = static_cast<int64_t>(std::floor(pos));
                if (i < 1 || i + 2 >= inCount || i <= inCount - inSize)
                    continue;
                const double f = pos - i;
                const float x0 = inRing[static_cast<size_t>((i - 1) & inMask)];
                const float x1 = inRing[static_cast<size_t>(i & inMask)];
                const float x2 = inRing[static_cast<size_t>((i + 1) & inMask)];
                const float x3 = inRing[static_cast<size_t>((i + 2) & inMask)];
                const double sample = x1 + 0.5*f*(x2-x0+f*(2*x0-5*x1+4*x2-x3
                                             + f*(3*(x1-x2)+x3-x0)));
                const double other = outRing[idx]/outWeight[idx];
                cross += sample*other;
                aa += sample*sample;
                bb += other*other;
                ++taps;
            }
            if (taps < 5 || aa < 1e-7 || bb < 1e-7) continue;
            const double correlation = cross/std::sqrt(aa*bb);
            if (offset == 0) centerScore = correlation;
            if (correlation > bestScore) { bestScore = correlation; bestShift = offset; }
        }
        // Only shift a grain if an independently observed overlap is clearly
        // better and strongly phase-compatible. Avoid chasing unvoiced noise.
        grainDiagnostics.overlapMatchScore = static_cast<float>(std::max(-1.0, bestScore));
        // Never apply an uncorrelated offset: it can create more roughness than
        // the phase drift we intended to repair. The gain/output clocks are
        // untouched. A new overlap candidate is considered only when it has
        // reliable phase evidence and is materially better than zero shift.
        if (centerScore > -1.5 && bestScore > 0.83 && bestScore > centerScore + 0.03)
        {
            grainDiagnostics.overlapShiftSamples = static_cast<float>(bestShift);
            return static_cast<double>(bestShift);
        }
        grainDiagnostics.overlapShiftSamples = 0.0f;
        return 0.0;
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
        // Avoid transcendental work in the common fully-preserved case and the
        // familiar preserve-off case; only automation's in-between frames need
        // the continuous log-frequency interpolation.
        const double preserveFactor = smoothedFormantPreserve == 1.0 ? 1.0
            : (smoothedFormantPreserve == 0.0 ? ratio
               : std::exp((1.0 - smoothedFormantPreserve) * std::log(ratio)));
        const double formantRate = preserveFactor * formantShiftRatio;
        double wo = periodF / formantRate;

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

        // Both modes schedule grains at a causal left edge; unavailable right
        // source samples come from previously recorded, pitch-matched cycles.
        // The prior Standard rule depended on callback-sized read-ahead.

        for (;;)
        {
            const double ta0 = nextTs - static_cast<double>(latency);

            // Pick the analysis mark (spaced one period apart) nearest to ta0
            while (markPos + 0.5 * periodF < ta0)
            {
                markPos += periodF;
                if (lowLatencyExperiment)
                {
                    const double expectedMark = markPos;
                    const double desiredMark = refinePitchMark(markPos, periodF);
                    // Bound single-grain peak hops: offset peaks can otherwise
                    // flip the waveform phase even when the pitch period is stable.
                    markPos = expectedMark + std::clamp(desiredMark - expectedMark,
                                                          -0.055 * periodF, 0.055 * periodF);
                    grainDiagnostics.markDelta = static_cast<float>(markPos - expectedMark);
                    lastRefinedMark = markPos;
                }
            }

            if (lowLatencyExperiment)
            {
                // Schedule a grain at its causal left edge rather than waiting
                // for an unobserved centre mark. Reuse the most recent voiced
                // cycle for source samples that have not arrived. A minimum of
                // three recorded cycles avoids reading stale ring contents.
                if (nextTs - wo > static_cast<double>(inCount - 1)
                    || inCount < static_cast<int64_t>(3.0 * periodF) + 8)
                    break;
            }
            else if (nextTs - wo > static_cast<double>(inCount - 1)
                || inCount < static_cast<int64_t>(1.5 * periodF) + 8)
                break;                                          // wait for more audio

            const double phaseShift = phaseLockEnabled ? phaseMatchOffset(markPos, periodF) : 0.0;
            if (phaseLockEnabled)
                grainDiagnostics.markDelta = static_cast<float>(phaseShift);
            const double  limit  = static_cast<double>(inCount) - 3.0;   // newest position the cubic can use
            const int64_t grainStart = static_cast<int64_t>(std::ceil(nextTs - wo));
            const int64_t oFirst = std::max(grainStart, outRead);
            const int64_t oLast  = static_cast<int64_t>(std::floor(nextTs + wo));
            // Compare candidate source phase with ALREADY SCHEDULED overlap.
            // This can identify destructive phase mismatch that recorded-history
            // correlation cannot see, without advancing either transport clock.
            grainDiagnostics.overlapMatchScore = 0.0f;
            grainDiagnostics.overlapShiftSamples = 0.0f;
            const double overlapShift = lowLatencyExperiment
                ? overlapPhaseOffset(nextTs, markPos + phaseShift, periodF,
                                     rate, wo, oFirst, oLast, limit)
                : 0.0;
            const bool startsLate = grainStart < outRead;

            // A two-dimensional Hann oscillator avoids the expensive cosine
            // call for *every* overlap sample. Reset phase once per grain so
            // numerical drift cannot accumulate between notes/host blocks.
            const double windowStep = static_cast<double>(piOverW);
            const double initialAngle = windowStep *
                static_cast<double>(static_cast<float>(static_cast<double>(oFirst) - nextTs + wo));
            const double rotC = std::cos(windowStep), rotS = std::sin(windowStep);
            double windowC = std::cos(initialAngle), windowS = std::sin(initialAngle);
            for (int64_t o = oFirst; o <= oLast; ++o)
            {
                float w = 0.5f * (1.0f - static_cast<float>(windowC));
                const double nextC = windowC * rotC - windowS * rotS;
                windowS = windowS * rotC + windowC * rotS;
                windowC = nextC;
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

                double pos = markPos + phaseShift + overlapShift + (static_cast<double>(o) - nextTs) * rate;
                // Calibrated phase anchor for proactive grain construction. This
                // compensates the 220 Hz neutral-tone phase advance introduced
                // by early scheduling. It is NOT a guarantee of constant
                // psychoacoustic delay at every pitch or correction ratio.
                if (lowLatencyExperiment)
                    pos -= kPredictivePhaseOffsetSeconds * sr;
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

                float smp = c0 * inRing[static_cast<size_t>((a - 1) & inMask)]
                          + c1 * inRing[static_cast<size_t>( a      & inMask)]
                          + c2 * inRing[static_cast<size_t>((a + 1) & inMask)]
                          + c3 * inRing[static_cast<size_t>((a + 2) & inMask)];

                if (lowLatencyExperiment)
                {
                    // The final available cycle must repeat when the right half
                    // of a predictive grain extends into unobserved audio. A
                    // hard splice between the end and start of that cycle can
                    // click when the detected period is a few samples imperfect.
                    // Approach the wrap with a short, source-domain crossfade.
                    const double fadeLength = std::min(0.08 * periodF, 0.00045 * sr);
                    if (fadeLength > 2.0 && pos > limit - fadeLength)
                    {
                        double older = pos - periodF;
                        if (older >= 2.0)
                        {
                            const double oldFloor = std::floor(older);
                            const int64_t oldIndex = static_cast<int64_t>(oldFloor);
                            const float f = static_cast<float>(older - oldFloor);
                            const float f2 = f*f, f3 = f2*f;
                            const float p0 = -0.5f*f3 + f2 - 0.5f*f;
                            const float p1 = 1.5f*f3 - 2.5f*f2 + 1.0f;
                            const float p2 = -1.5f*f3 + 2.0f*f2 + 0.5f*f;
                            const float p3 = 0.5f*f3 - 0.5f*f2;
                            const float sampleOld =
                                  p0*inRing[static_cast<size_t>((oldIndex-1)&inMask)]
                                + p1*inRing[static_cast<size_t>(oldIndex&inMask)]
                                + p2*inRing[static_cast<size_t>((oldIndex+1)&inMask)]
                                + p3*inRing[static_cast<size_t>((oldIndex+2)&inMask)];
                            const float u = static_cast<float>(std::clamp(
                                (pos - (limit - fadeLength))/fadeLength, 0.0, 1.0));
                            const float blend = u*u*(3.0f - 2.0f*u);
                            smp += (sampleOld - smp)*blend;
                        }
                    }
                }

                const size_t outputIndex = static_cast<size_t>(o & outMask);
                if (outRingEpoch[outputIndex] != ringEpoch)
                {
                    outRing[outputIndex] = 0.0f;
                    outWeight[outputIndex] = 0.0f;
                    outAbsContribution[outputIndex] = 0.0f;
                    outContributors[outputIndex] = 0;
                    outRingEpoch[outputIndex] = ringEpoch;
                }
                const float grain = smp * w * gain;
                outRing[outputIndex] += grain;
                if (detailedDiagnostics)
                    outAbsContribution[outputIndex] += std::abs(grain);
                if (lowLatencyExperiment)
                {
                    outWeight[outputIndex] += w * gain;
                    if (outContributors[outputIndex] < 255) ++outContributors[outputIndex];
                }
            }

            nextTs += hop;
        }

        // Continuous equal-latency voicing handoff. Causal one-pole smoothing is
        // sample-based, not host-block-based. A 2.5 ms release exposes consonants
        // quickly; a 4 ms return to the wet path avoids clipping vowel attacks.
        const float dryCoefficient = dryMixTarget > dryMix
            ? dryReleaseCoeff : dryAttackCoeff;
        for (int i = 0; i < m; ++i)
        {
            dryMix = dryMixTarget + (dryMix - dryMixTarget) * dryCoefficient;
            if (std::abs(dryMix - dryMixTarget) < 1.0e-6f)
                dryMix = dryMixTarget;
            const int64_t outputTime = outRead + i;
            const size_t idx = static_cast<size_t>(outputTime & outMask);
            const bool valid = outRingEpoch[idx] == ringEpoch;
            float shifted = valid ? outRing[idx] : 0.0f;
            if (valid)
            {
                if (lowLatencyExperiment)
                {
                    // Hann gain assumes perfect grain spacing. When predictive
                    // grains arrive late, their accumulated window sum can be
                    // far below one. Divide by measured coverage, NOT a fixed
                    // output boost. Limit compensation to avoid explosive gain
                    // on a nearly empty / transient window.
                    if (detailedDiagnostics && outAbsContribution[idx] > 1.0e-7f)
                    {
                        diagnosticCancellationSum += std::clamp(
                            1.0f - std::abs(shifted) / outAbsContribution[idx], 0.0f, 1.0f);
                        ++diagnosticCancellationCount;
                    }
                    const float coverage = outWeight[idx];
                    const float gainCorrection = 1.0f / std::clamp(coverage, 0.25f, 1.50f);
                    shifted *= gainCorrection;
                    grainDiagnostics.coverage = coverage;
                    grainDiagnostics.appliedGain = gainCorrection;
                    grainDiagnostics.contributors = outContributors[idx];
                    diagnosticCoverageSum += coverage;
                    diagnosticGainSum += gainCorrection;
                    diagnosticContributorsSum += outContributors[idx];
                    ++diagnosticCount;
                    outWeight[idx] = 0.0f;
                    outContributors[idx] = 0;
                }
                outRing[idx] = 0.0f;
                if (detailedDiagnostics) outAbsContribution[idx] = 0.0f;
            }
            // Do not blend a shifted voiced cycle with a phase-unrelated dry
            // cycle during pitch jumps. Such blends caused destructive level
            // interference on measured real-vocal transitions. The existing
            // voiced/unvoiced dryMix still preserves consonants and breaths.
            float result = shifted;
            if (dryMix > 0.0f)
            {
                const int64_t dryTime = outputTime - latency;
                const float dry = dryTime >= 0 && dryTime < inCount
                    ? inRing[static_cast<size_t>(dryTime & inMask)] : 0.0f;
                result = result * (1.0f - dryMix) + dry * dryMix;
                // During a wet/dry handoff the two vocal waveforms can be out
                // of phase. A linear crossfade loses up to 3 dB when they are
                // uncorrelated; use their causal short-time covariance to
                // preserve interpolated energy. Never apply this to stable wet
                // tuning, never apply a fixed output boost, and bound the gain.
                if (lowLatencyExperiment && energyMatchedHandoff && dryMix > 0.005f && dryMix < 0.995f)
                {
                    const float coeff = crossfadeEnergyCoeff;
                    crossfadeWetPower = coeff * crossfadeWetPower + (1.0f - coeff) * shifted * shifted;
                    crossfadeDryPower = coeff * crossfadeDryPower + (1.0f - coeff) * dry * dry;
                    crossfadeCov = coeff * crossfadeCov + (1.0f - coeff) * shifted * dry;
                    const float a = 1.0f - dryMix, b = dryMix;
                    const float desired = a*crossfadeWetPower + b*crossfadeDryPower;
                    const float actual = a*a*crossfadeWetPower + b*b*crossfadeDryPower
                        + 2.0f*a*b*crossfadeCov;
                    const float gain = desired > 1.0e-7f && actual > 1.0e-7f
                        ? std::clamp(std::sqrt(desired / actual), 1.0f, 1.35f) : 1.0f;
                    // Taper correction to unity at either endpoint, avoiding a
                    // sudden gain step when the handoff reaches fully dry/wet.
                    const float shapedGain = 1.0f + 4.0f*a*b*(gain - 1.0f);
                    crossfadeGain = shapedGain + crossfadeGainCoeff
                        * (crossfadeGain - shapedGain);
                    result *= crossfadeGain;
                }
                else
                {
                    crossfadeGain = 1.0f;
                    crossfadeWetPower = crossfadeDryPower = crossfadeCov = 0.0f;
                }
            }
            else if (lowLatencyExperiment)
            {
                crossfadeGain = 1.0f;
                crossfadeWetPower = crossfadeDryPower = crossfadeCov = 0.0f;
            }
            grainDiagnostics.maxOutputStep = std::max(grainDiagnostics.maxOutputStep,
                                                        std::abs(result - lastOutput));
            lastOutput = result;
            data[i] = result;
        }
        outRead += m;
    }

    double sr = 44100.0;
    int maxPeriod = 800, minPeriod = 32, latency = 2000;
    int inSize = 0, outSize = 0;
    int64_t inMask = 0, outMask = 0;

    std::vector<float> inRing, outRing, outWeight, outAbsContribution;
    std::vector<uint8_t> outContributors;
    GrainDiagnostics grainDiagnostics;
    float lastOutput = 0.0f;
    float diagnosticCoverageSum = 0.0f, diagnosticGainSum = 0.0f;
    int diagnosticContributorsSum = 0, diagnosticCount = 0;
    float diagnosticCancellationSum = 0.0f;
    int diagnosticCancellationCount = 0;
    bool detailedDiagnostics = false;
    std::vector<uint64_t> outRingEpoch;
    uint64_t ringEpoch = 0;

    int64_t inCount = 0;
    int64_t outRead = 0;
    double  nextTs  = 0.0;
    double  markPos = 0.0;
    double previousPhaseAnchor = -1.0e12;
    double previousPhaseOffset = 0.0;
    float phaseScore = 0.0f;
    double  lastRefinedMark = -1.0e12;
    double  lastPeriod = 220.0;
    double  smoothedPeriod = 220.0;
    double  lowLatencySlicePeriod = 220.0;
    bool    formantPreserve = true;
    double  smoothedFormantPreserve = 1.0;
    float   dryMix = 0.0f;
    float   dryMixTarget = 0.0f;
    float   dryReleaseCoeff = 0.99f;
    float   dryAttackCoeff = 0.995f;
    float crossfadeEnergyCoeff = 0.99f, crossfadeGainCoeff = 0.99f;
    float crossfadeWetPower = 0.0f, crossfadeDryPower = 0.0f, crossfadeCov = 0.0f;
    float crossfadeGain = 1.0f;
    bool energyMatchedHandoff = true;
    bool    lowLatencyExperiment = false;
    bool phaseLockEnabled = false;
    double  lowLatencyBudgetMs = 3.5;
    double  formantShiftRatioTarget = 1.0;
    double  smoothedFormantShiftRatio = 1.0;
};

} // namespace humtune
