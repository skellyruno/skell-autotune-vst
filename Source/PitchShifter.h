#pragma once
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#include <JuceHeader.h>
#ifndef SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
#define SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT 0
#endif
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
#include "EpochWhitening.h"
#include "EpochObservation.h"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>

namespace humtune
{

class PitchShifter
{
    friend struct PitchShifterSpectralOracle;
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
        // These controls advance on fixed sample slices, independent of host block size.
        periodSliceCoefficient = std::exp(-static_cast<double>(kLowLatencyChunk) / (sr * 0.0025));
        colourSliceCoefficient = std::exp(-static_cast<double>(kFormantUpdateChunk)
                                          / (sr * kFormantShiftSmoothingSeconds));
        preserveSliceCoefficient = std::exp(-static_cast<double>(kFormantUpdateChunk)
                                            / (std::max(1.0, sr) * kFormantShiftSmoothingSeconds));
        static_assert(kLowLatencyChunk == kFormantUpdateChunk);
        epochRadiusLimit=std::max(2,static_cast<int>(std::lround(48.0*sr/44100.0)));
        epochSpanLimit=std::max(24,static_cast<int>(std::lround(512.0*sr/44100.0)));
        epochSampleStride=std::max(1,static_cast<int>(std::lround(sr/16000.0)));
        // One fixed reference fingerprint per correlation search. prepare() owns
        // its storage; a search overwrites only the bounded taps it will use.
        epochReference.resize(static_cast<size_t>(std::max(epochSpanLimit,512)));
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
        epochWhitening.prepare(sr,epochSpanLimit,epochRadiusLimit);
#endif
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

        liveCoefficientData.assign(3*kLiveCoefficientStride,0.0f);
        liveReal.assign(kLiveFFT/2,0.0f);liveImag.assign(kLiveFFT/2,0.0f);
        liveWaveKernel.resize(1025*32);
        for(int phase=0;phase<=1024;++phase) {
            const double f=static_cast<double>(phase)/1024;
            double sum=0;
            for(int k=0;k<32;++k) {
                const double x=k-15-f;
                const double sinc=std::abs(x)<1e-12 ? 1.0 : std::sin(3.141592653589793*x)/(3.141592653589793*x);
                const double window=std::abs(x)>=16 ? 0 : .42+.5*std::cos(3.141592653589793*x/16)+.08*std::cos(2*3.141592653589793*x/16);
                const double value=sinc*window;
                liveWaveKernel[phase*32+k]=static_cast<float>(value);sum+=value;
            }
            for(int k=0;k<32;++k)liveWaveKernel[phase*32+k]/=static_cast<float>(sum);
        }
        liveWaveTables.resize(3*kLiveFFT);
        liveWaveBounds.resize(3*2*(kLiveHarmonics+1));liveWaveSize.fill(0);liveWaveBoundCount.fill(-1);liveWaveHz.fill(0);
        liveTwiddleReal.resize(kLiveFFT/2);liveTwiddleImag.resize(kLiveFFT/2);
        liveHann.resize(kLiveWindow+1);
        for(int i=0;i<kLiveFFT/2;++i)
        {
            const double angle=-2.0*3.141592653589793*i/kLiveFFT;
            liveTwiddleReal[static_cast<size_t>(i)]=static_cast<float>(std::cos(angle));
            liveTwiddleImag[static_cast<size_t>(i)]=static_cast<float>(std::sin(angle));
        }
        for(int i=0;i<=kLiveWindow;++i)
            liveHann[static_cast<size_t>(i)]=static_cast<float>(0.5-0.5*std::cos(2.0*3.141592653589793*i/kLiveWindow));
        liveStageReal.resize(kLiveFFT/2);liveStageImag.resize(kLiveFFT/2);
        for(int length=2;length<=kLiveFFT/2;length*=2) {
            const int half=length/2,step=kLiveFFT/length;
            for(int k=0;k<half;++k) {
                liveStageReal[half-1+k]=liveTwiddleReal[k*step];
                liveStageImag[half-1+k]=liveTwiddleImag[k*step];
            }
        }
        liveBitReversal.resize(kLiveFFT/2);
        for(int i=0;i<kLiveFFT/2;++i) {
            int input=i,reversed=0;
            for(int bit=kLiveFFT/4;bit>0;bit>>=1) {reversed=(reversed<<1)|(input&1);input>>=1;}
            liveBitReversal[i]=static_cast<uint16_t>(reversed);
        }
        liveBlendStep=1.0/std::max(1.0,sr*0.002);
        // A phase-continuous clock still creates sharp slope changes at target updates.
        // Standard uses a 0.25-ms slew to soften that curvature; the explicit live
        // mode retains its established 0.15-ms response. Neither changes transport delay.
        liveLowFrequencyCoefficient=std::exp(-1.0/(sr*kLowLatencyFrequencySlewSeconds));
        liveStandardFrequencyCoefficient=std::exp(-1.0/(sr*kStandardFrequencySlewSeconds));
        liveFrequencyCoefficient=lowLatencyExperiment ? liveLowFrequencyCoefficient : liveStandardFrequencyCoefficient;
        liveControlSamples=std::max(16,static_cast<int>(sr*0.002));
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
    // Offline A/B gate; source epochs use recorded audio only.
    void setSourceEpochTrackingEnabled(bool on) noexcept { sourceEpochTracking=on; }
    // Stereo predictive synthesis must not make unrelated peak choices for
    // nearly identical channels. The first channel owns the pitch-mark clock;
    // the second uses the same mark position but reads *its own* waveform.
    // This fixed-capacity bridge is written and consumed on the audio thread;
    // it performs no allocation, locking, or inter-thread communication.
    struct PhaseMarkLink
    {
        struct Entry { uint64_t grain = UINT64_MAX; double mark = 0.0; double period = 0.0; int64_t when = 0; uint64_t epoch = 0; double sourceEpoch = 0.0; double phase=0,hop=0,rate=1; float match=0; };
        static constexpr size_t capacity = 1024;
        std::array<Entry, capacity> history{};
    };

    void setPhaseMarkLink(PhaseMarkLink* link, bool leader) noexcept
    {
        sharedMarks = link;
        phaseLeader = leader;
    }

    // Opt-in expensive trace instrumentation. Disabled in native DAW processing.
    void setDetailedDiagnosticsEnabled(bool enabled) noexcept { detailedDiagnostics = enabled; }
    // Internal A/B gate for deterministic regression testing. Both modes use
    // a bounded, covariance-aware handoff at voiced/unvoiced boundaries.
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
        liveFrequencyCoefficient = on ? liveLowFrequencyCoefficient : liveStandardFrequencyCoefficient;
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
        emittedGrains = 0;
        liveBackendActive=false;liveSpectrumValid=false;livePhaseInitialized=false;
        liveWaveSize.fill(0);liveWaveBoundCount.fill(-1);liveWaveClockValid=false;liveWaveStableSince=0;
        liveCurrentIndex=0;liveOldIndex=1;liveSpareIndex=2;liveCurrentCount=liveOldCount=0;
        liveSpectrumBlend=1;livePhase=0;liveFrequency=200;livePreviousRotation=0;
        liveModelKey = {};
        liveOldSourceCenter=liveCurrentSourceCenter=0;liveModelSourceGain=1;
        liveLastSourceEpoch=-1e12;liveLastPreserve=-1;liveLastColour=-1;liveLastHop=-1;liveLastBuild=-1000000;
        recordedEpochValid = false; recordedEpoch = recordedPeriod = 0.0;
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
        epochWhitening.reset();epochObservation.reset();
#endif
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
        handoffCrest = 0; handoffCrestHold = 0;
        grainDiagnostics = {};
        lastOutput = 0.0f;
    }

    void setSourcePitchObservation(float hz,double age,float confidence) noexcept {
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
        epochObservation.observe(hz,age,confidence,static_cast<double>(inCount-1),sr);
#else
        (void) hz; (void) age; (void) confidence;
#endif
    }
    int getLatencySamples() const { return latency; }
    struct SourceCycleDiagnostics
    {
        float period = 0, confidence = 0, ageSeconds = 0;
    };
    SourceCycleDiagnostics getSourceCycleDiagnostics () const noexcept
    {
        if (!phaseLockEnabled || !sourceEpochTracking || !recordedEpochValid)
            return {};
        return {static_cast<float> (recordedPeriod), recordedEpochConfidence,
                static_cast<float> (liveSpectrumValid
                                        ? (inCount - (liveOldSourceCenter * (1 - liveSpectrumBlend) +
                                                      liveCurrentSourceCenter * liveSpectrumBlend)) /
                                              sr
                                        : 0.0)};
    }

    void process (float* data, int numSamples, float periodSamples, float shiftRatio)
    {
        if (numSamples <= 0 || inRing.empty())
            return;

        const bool spectrumBackend=phaseLockEnabled && sourceEpochTracking;
        if(spectrumBackend!=liveBackendActive && inCount>0) reset();
        liveBackendActive=spectrumBackend;
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

        if (lowLatencyExperiment || spectrumBackend)
        {
            // Update the predictor period on a persistent 32-sample timeline, rather
            // than once per host call. The DSP work below is still scheduled sample by
            // sample, so decisions are causal and callback-size invariant; holding the
            // period geometry for each globally aligned slice avoids the bass instability
            // seen when moving the grain spacing by tiny amounts every sample.
            const double smoothingCoeff = periodSliceCoefficient;
            const double formantCoeff = colourSliceCoefficient;
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
                    smoothFormantPreserve();
                }
                if(spectrumBackend) data[i]=processLiveSpectrumSample(data[i],targetPeriod,ratio);
                else processChunk(data + i, 1, lowLatencySlicePeriod, ratio, smoothedFormantShiftRatio);
            }
        }
        else if (formantShiftIsActive() || formantPreserveIsTransitioning())
        {
            // Use a persistent 32-sample timeline for the new control. Automation
            // and host block boundaries therefore cannot change its smoothing rate.
            // The neutral setting stays on the old 256-sample path below to keep
            // existing presets/renders bit-identical.
            const double formantCoeff = colourSliceCoefficient;
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
                    smoothFormantPreserve();
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
        if ((lowLatencyExperiment || spectrumBackend) && diagnosticCount > 0)
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
    static constexpr double kStandardFrequencySlewSeconds = 0.00025;
    static constexpr double kLowLatencyFrequencySlewSeconds = 0.00015;
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

    void smoothFormantPreserve() noexcept
    {
        const double target = formantPreserve ? 1.0 : 0.0;
        if (smoothedFormantPreserve == target)
            return;
        const double coeff = preserveSliceCoefficient;
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

    // Track the phase of successive recorded source cycles. The comparison
    // ends at the candidate epoch, so no future waveform is needed.
    double correlateSourceEpoch(double expected, double previous, double period) noexcept
    {
        candidateEpochConfidence=0.0f;
        const bool live=phaseLockEnabled && sourceEpochTracking;
        const int radius = std::clamp(static_cast<int>(0.08 * period), 2, live ? epochRadiusLimit : 48);
        const int span = std::clamp(static_cast<int>(0.8 * period), 24, live ? epochSpanLimit : 512);
        const int stride = live ? epochSampleStride : 3;
        const int64_t oldest = std::max<int64_t>(1, inCount - inSize + 2);
        const int64_t newest = inCount - 3;
        if (previous - span < oldest || previous > newest
            || expected + radius > newest || expected - radius - span < oldest)
            return expected;
        const auto read = [this](double x) noexcept {
            const auto i = static_cast<int64_t>(std::floor(x));
            const double f = x - i;
            return inRing[static_cast<size_t>(i & inMask)] * (1.0 - f)
                 + inRing[static_cast<size_t>((i + 1) & inMask)] * f;
        };
        // The previous-cycle fingerprint and its energy are identical for all
        // candidate shifts, including the two sub-sample refinement queries.
        // Preserve tap order and the original read arithmetic exactly.
        double referencePower=0;
        int tapCount=0;
        for(int j=0;j<span;j+=stride) {
            const double b=read(previous-j);
            epochReference[static_cast<size_t>(tapCount++)]=b;
            referencePower+=b*b;
        }
        const auto score = [&](int shift) noexcept {
            double xx = 0, xy = 0;
            int tap=0;
            for (int j = 0; j < span; j += stride)
            {
                const double a = read(expected + shift - j), b = epochReference[static_cast<size_t>(tap++)];
                xx += a*a; xy += a*b;
            }
            return xx > 1e-7 && referencePower > 1e-7 ? xy / std::sqrt(xx*referencePower) : -1.0;
        };
        double best = -2.0;
        int offset = 0;
        for (int shift = -radius; shift <= radius; ++shift)
        {
            const double c = score(shift);
            if (c > best) { best = c; offset = shift; }
        }
        // A boundary maximum cannot establish a local phase peak. Weak matches
        // retain the detector trajectory instead of imposing a guessed phase.
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
        double rawDistance=0;
        if(best>.95 && std::abs(offset)<radius) {
            const double a=score(offset-1),b=score(offset+1),curve=a-2*best+b;
            const double fraction=curve<-1e-8 ? std::clamp(.5*(a-b)/curve,-.5,.5) : 0.;
            const double rawPeriod=expected+offset+fraction-previous;
            const int lastJ=((span-1)/stride)*stride;
            const double midpoint=.5*(expected+offset+fraction+previous-lastJ);
            const double reference=lowLatencyExperiment ? epochObservation.periodAt(midpoint,period,sr) : period;
            if(rawPeriod>0 && reference>0)rawDistance=std::abs(1200*std::log2(rawPeriod/reference));
        }
        // A reliable waveform match that disagrees with the independent pitch
        // hint can be biased by moving vocal-tract resonances. Use residual
        // evidence only when it resolves that disagreement by at least5c.
        if(rawDistance>10 && epochWhitening.prepareMatch(previous,expected,span,radius,stride,inCount)) {
            const auto whitenedScore=[&](int shift) noexcept {
                return epochWhitening.matchScore(shift);
            };
            int whiteOffset=0;double whiteBest=-2;
            for(int shift=-radius;shift<=radius;++shift) {const double value=whitenedScore(shift);if(value>whiteBest){whiteBest=value;whiteOffset=shift;}}
            if(whiteBest>.9 && std::abs(whiteOffset)<radius && score(whiteOffset)>.9) {
                const double lo=whitenedScore(whiteOffset-1),hi=whitenedScore(whiteOffset+1),curve=lo-2*whiteBest+hi;
                const double fraction=curve<-1e-8 ? std::clamp(.5*(lo-hi)/curve,-.5,.5) : 0.;
                const double whitePeriod=expected+whiteOffset+fraction-previous;
                const int lastJ=((span-1)/stride)*stride;
                const double midpoint=.5*(expected+whiteOffset+fraction+previous-lastJ)-epochWhitening.delaySamples();
                const double reference=lowLatencyExperiment ? epochObservation.periodAt(midpoint,period,sr) : period;
                const double whiteDistance=whitePeriod>0 && reference>0 ? std::abs(1200*std::log2(whitePeriod/reference)) : 1e6;
                if(whiteDistance+5<rawDistance) {
                    candidateEpochConfidence=static_cast<float>(score(whiteOffset));
                    return expected+whiteOffset+fraction;
                }
            }
        }
#endif
        if (best < 0.85 || std::abs(offset) == radius) return expected;
        candidateEpochConfidence=static_cast<float>(best);
        double fractional = 0.0;
        const double lo = score(offset - 1), hi = score(offset + 1);
        const double curvature = lo - 2*best + hi;
        if (curvature < -1e-8)
            fractional = std::clamp(0.5*(lo - hi)/curvature, -0.5, 0.5);
        return expected + offset + fractional;
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
        // Standard uses a longer quality-first history window; both modes keep their bounded phase slew.
        // v39: Longer historical phase fingerprint on the quality-focused Standard
        // path rejects mismatched harmonic lobes on real vocals. Experimental
        // Low Latency keeps its verified 36-sample window unchanged.
        // Every candidate remains guarded by oldestValid/newestValid below.
        const int window = lowLatencyExperiment ? 36 : 72;
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
        // Maintain a phase-continuous source anchor with a 1.8%-of-period
        // Standard slew cap. The Low Latency synthesis clock remains unchanged.
        const double maxPhaseStep = (lowLatencyExperiment ? 0.020 : 0.018) * period;
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

    // Standard uses already recorded audio near its delayed source timestamp;
    // Low Latency uses the newest complete cycle. Both retain a recorded-only
    // correlation fingerprint and reject unsupported / boundary phase matches.
    void updateRecordedEpoch (double prior, bool rapidBootstrap = false) noexcept
    {
        const double latest = lowLatencyExperiment ? static_cast<double> (inCount - 3)
                                                   : std::min (static_cast<double> (inCount - 3),
                                                               inCount - latency + 1.5 * prior);
        const double radius = std::clamp (static_cast<int> (0.08 * prior), 2, epochRadiusLimit);
        if (!recordedEpochValid || latest - recordedEpoch > 5.0 * prior)
        {
            if (latest < (rapidBootstrap ? 2.0 * prior + radius + 8.0 : 3.0 * prior + 8.0))
                return;
            // Pick a recorded high-energy point within one complete cycle.
            // The PSOLA basis needs a stable excitation-centred window; an
            // arbitrary phase derived from the PDC budget changes its envelope.
            // The output oscillator explicitly accounts for this epoch when it
            // initializes, so neutral wet phase still matches reported latency.
            const int64_t end = static_cast<int64_t> (std::floor (latest - radius));
            const int64_t begin = std::max<int64_t> (1, end - static_cast<int64_t> (std::ceil (prior)));
            int64_t peak = begin;
            double value = -1;
            for (int64_t j = begin; j <= end; ++j)
            {
                const double amplitude = std::abs (inRing[static_cast<size_t> (j & inMask)]);
                if (amplitude > value)
                {
                    value = amplitude;
                    peak = j;
                }
            }
            double seed = static_cast<double> (peak);
            if (peak > begin && peak < end)
            {
                const double a = std::abs (inRing[static_cast<size_t> ((peak - 1) & inMask)]);
                const double b = std::abs (inRing[static_cast<size_t> (peak & inMask)]);
                const double c = std::abs (inRing[static_cast<size_t> ((peak + 1) & inMask)]);
                const double curve = a - 2 * b + c;
                if (curve < -1e-12)
                    seed += std::clamp (.5 * (a - c) / curve, -.5, .5);
            }
            const double previous = seed - prior;
            recordedEpoch = correlateSourceEpoch (previous + prior, previous, prior);
            recordedPeriod = recordedEpoch - previous;
            recordedEpochConfidence = candidateEpochConfidence;
            recordedEpochValid = true;
        }
        for (int catchup = 0; catchup < 4; ++catchup)
        {
            const double expected = recordedEpoch + prior;
            if (expected + radius > latest)
                break;
            const double observed = correlateSourceEpoch (expected, recordedEpoch, prior);
            recordedPeriod = std::clamp (observed - recordedEpoch, 0.8 * prior, 1.2 * prior);
            recordedEpoch = observed;
            recordedEpochConfidence = candidateEpochConfidence;
        }
    }

    // Phase-continuous periodic-cycle backend. A single target oscillator is
    // independent of recorded source epochs. Formants change the source-domain
    // read rate of a normalized PSOLA basis, never its musical output clock.
    // FFT scratch/coefficient banks/twiddles are allocated only in prepare().
    // Three coefficient banks preserve the currently audible interpolation when
    // another source cycle arrives during a morph. Stereo shares geometry and
    // one whole-waveform phase rotation, never either channel's audio samples.
    static constexpr int kLiveFFT = 8192;
    static constexpr int kLiveHarmonics = 2048;
    static constexpr int kLiveCoefficientStride = 2 * (kLiveHarmonics + 1);
    static constexpr int kLiveWindow = 2048;

    float *liveCoefficients (int index) noexcept
    {
        return liveCoefficientData.data () + index * kLiveCoefficientStride;
    }
    const float *liveCoefficients (int index) const noexcept
    {
        return liveCoefficientData.data () + index * kLiveCoefficientStride;
    }

    float readKnownSource (double position) const noexcept
    {
        const auto i = static_cast<int64_t> (std::floor (position));
        if (i < 1 || i + 2 >= inCount || i - 1 < inCount - inSize)
            return 0.0f;
        const double f = position - i;
        const double a = inRing[static_cast<size_t> ((i - 1) & inMask)];
        const double b = inRing[static_cast<size_t> (i & inMask)];
        const double c = inRing[static_cast<size_t> ((i + 1) & inMask)];
        const double d = inRing[static_cast<size_t> ((i + 2) & inMask)];
        return static_cast<float> (
            b + 0.5 * f * (c - a + f * (2 * a - 5 * b + 4 * c - d + f * (3 * (b - c) + d - a))));
    }
    float readCompleteCycle (double position, double epoch, double period,
                             double inversePeriod, double fade) const noexcept
    {
        position -= std::floor ((position - (epoch - period)) * inversePeriod) * period;
        float value = readKnownSource (position);
        if (fade > 2.0 && position > epoch - fade)
        {
            const float old = readKnownSource (position - period);
            const double u = (position - (epoch - fade)) / fade;
            value += static_cast<float> ((old - value) * u * u * (3.0 - 2.0 * u));
        }
        return value;
    }
    double liveWindow (double u) const noexcept
    {
        if (std::abs (u) >= 1.0)
            return 0.0;
        const double position = (u + 1.0) * 0.5 * kLiveWindow;
        // u can be just below one while the multiply rounds to the exact
        // table endpoint. Keep both interpolation taps inside the allocation.
        const int i = std::min (kLiveWindow - 1, static_cast<int> (position));
        const double f = position - i;
        return liveHann[static_cast<size_t> (i)] * (1.0 - f) + liveHann[static_cast<size_t> (i + 1)] * f;
    }
    void livePackedFFT (int count) noexcept
    {
        // Even real samples and odd imaginary samples avoid clearing a complex
        // analysis buffer. The postpass below recovers the real FFT spectrum.
        int shift=0;
        for(int n=count;n<kLiveFFT/2;n*=2)++shift;
        for(int i=1;i<count;++i) {
            const int j=liveBitReversal[i]>>shift;
            if(i<j) {std::swap(liveReal[i],liveReal[j]);std::swap(liveImag[i],liveImag[j]);}
        }
        float* __restrict real=liveReal.data();
        float* __restrict imag=liveImag.data();
        const float* __restrict stageReal=liveStageReal.data();
        const float* __restrict stageImag=liveStageImag.data();
        for(int length=2;length<=count;length*=2) {
            const int half=length/2;
            const auto* c=stageReal+half-1;
            const auto* s=stageImag+half-1;
            for(int start=0;start<count;start+=length) {
                for(int k=0;k<half;++k) {
                    const int a=start+k,b=a+half;
                    const float re=c[k]*real[b]-s[k]*imag[b];
                    const float im=c[k]*imag[b]+s[k]*real[b];
                    const float ar=real[a],ai=imag[a];
                    real[a]=ar+re;imag[a]=ai+im;
                    real[b]=ar-re;imag[b]=ai-im;
                }
            }
        }
    }
    double buildLiveSpectrum (double epoch, double period, double hop, double rate,
                              double forcedRotation = std::numeric_limits<double>::quiet_NaN ()) noexcept
    {
        const double outputHz = sr / hop, band = std::min (24000.0, 0.48 * sr);
        const int need = std::max (128, static_cast<int> (std::ceil ((0.5 * sr * rate + band) / outputHz)));
        const int size = std::min (kLiveFFT, nextPow2 (need)), packed = size / 2;
        // Ignore only sub-millicent model-input jitter on a completed, cached
        // copy of the same recorded cycle. The caller still admits the control
        // event and advances its scheduling clock/linked mark history normally.
        // The output oscillator continues to use every wanted-frequency update.
        const int currentSize = liveCurrentCount >= kLiveHarmonics
                                  ? kLiveFFT : 2 * (liveCurrentCount + 1);
        if (liveSpectrumValid && liveSpectrumBlend >= 1.0 - 1e-12
            && epoch == liveModelKey.epoch && period == liveModelKey.period
            && size == currentSize && liveModelKey.hop > 0 && liveModelKey.rate > 0
            && std::abs(hop - liveModelKey.hop) <= 2e-7 * liveModelKey.hop
            && std::abs(rate - liveModelKey.rate) <= 2e-7 * liveModelKey.rate
            && (!std::isfinite(forcedRotation) || forcedRotation == livePreviousRotation)
            && liveWaveAcceptsFrequency(liveCurrentIndex, outputHz))
        {
            liveSpectrumBlend = 1.0;
            // Preserve the existing wave-clock qualification even when the
            // expensive source model can be reused. Its original input key
            // stays fixed so small changes cannot accumulate without a rebuild.
            if(!liveWaveClockValid || std::abs(liveWaveLastWantedHz-outputHz)>2e-7*std::max(1.0,outputHz)) {
                liveWaveClockValid=true;liveWaveStableSince=inCount;
            }
            liveWaveLastWantedHz=outputHz;
            return livePreviousRotation;
        }
        const double width = std::max (period / rate, 0.55 * hop);
        const int span = std::min (128, static_cast<int> (std::ceil (width / hop)) + 1);
        // The source cycle and overlap geometry are fixed for this model.
        // Hoist their divisions and seam envelope out of the sample loops.
        const double inversePeriod = 1.0 / period;
        const double windowScale = hop / width;
        const double fade = std::min ({0.08 * period, 0.00045 * sr,
                                       std::max (0.0, epoch - period - 2.0)});
        double energy = 0, coverage = 0, contributors = 0, cancellation = 0, sourceEnergy = 0, sourcePeak = 0,
               modelPeak = 0;
        // With unchanged formant colour, source rate maps one output cycle
        // to exactly one recorded period. Every overlapping grain is then the
        // same periodic sample; normalized Hann weights cancel analytically.
        // Keep diagnostics on the reference route so their work/coverage
        // accounting remains unchanged. This gate is model geometry, not a
        // signal-level heuristic; non-neutral colour/formant models fall back.
        const bool directCycle = !detailedDiagnostics
            && std::abs(hop * rate - period) <= 1e-12 * period;
        for (int i = 0; i < size; ++i)
        {
            const double phase = static_cast<double> (i) / size;
            const double source = readCompleteCycle (epoch + (phase - 1.0) * period, epoch, period, inversePeriod, fade);
            double sum = 0, weight = 0, absolute = 0;
            int count = 0;
            if (!directCycle) for (int k = -span; k <= span; ++k)
            {
                const double offset = (phase + k) * hop;
                const double w = liveWindow ((phase + k) * windowScale);
                if (w <= 1.0e-12)
                    continue;
                const double sample = readCompleteCycle (epoch + offset * rate, epoch, period, inversePeriod, fade);
                sum += w * sample;
                weight += w;
                if (detailedDiagnostics)
                    absolute += std::abs (w * sample);
                ++count;
            }
            const float value = directCycle ? static_cast<float>(source) : weight > 1.0e-12 ? static_cast<float> (sum / weight) : 0.0f;
            if (i & 1)
                liveImag[static_cast<size_t> (i / 2)] = value;
            else
                liveReal[static_cast<size_t> (i / 2)] = value;
            sourceEnergy += source * source;
            sourcePeak = std::max (sourcePeak, std::abs (source));
            modelPeak = std::max (modelPeak, static_cast<double> (std::abs (value)));
            energy += static_cast<double> (value) * value;
            coverage += directCycle ? 1.0 : weight * hop / width;
            contributors += directCycle ? 2 : count;
            if (detailedDiagnostics && absolute > 1.0e-12)
                cancellation += std::clamp (1.0 - std::abs (sum) / absolute, 0.0, 1.0);
        }
        const bool morph = liveSpectrumValid && dryMix < 0.999f;
        if (morph)
        {
            const auto *a = liveCoefficients (liveOldIndex);
            const auto *b = liveCoefficients (liveCurrentIndex);
            auto *mixed = liveCoefficients (liveSpareIndex);
            const bool currentOnly=liveSpectrumBlend>=1,oldOnly=liveSpectrumBlend<=0;
            const int count=currentOnly ? liveCurrentCount : oldOnly ? liveOldCount : std::max(liveOldCount,liveCurrentCount);
            if(currentOnly || oldOnly) {
                const auto* source=currentOnly ? b : a;
                std::copy_n(source,count+1,mixed);
                std::copy_n(source+kLiveHarmonics+1,count+1,mixed+kLiveHarmonics+1);
            }
            else for (int h = 0; h <= count; ++h)
            {
                const double ar = h <= liveOldCount ? a[h] : 0.0;
                const double ai = h <= liveOldCount ? a[kLiveHarmonics + 1 + h] : 0.0;
                const double br = h <= liveCurrentCount ? b[h] : 0.0;
                const double bi = h <= liveCurrentCount ? b[kLiveHarmonics + 1 + h] : 0.0;
                mixed[h] = static_cast<float> (ar * (1 - liveSpectrumBlend) + br * liveSpectrumBlend);
                mixed[kLiveHarmonics + 1 + h] =
                    static_cast<float> (ai * (1 - liveSpectrumBlend) + bi * liveSpectrumBlend);
            }
            const int endpoint=currentOnly ? liveCurrentIndex : oldOnly ? liveOldIndex : -1;
            if(endpoint>=0 && liveWaveSize[endpoint]>0) {
                // At a completed morph the other bank has zero weight. Preserve
                // the audible bank's table even when their reference Hz differ.
                const int n=liveWaveSize[endpoint];
                std::copy_n(liveWaveTables.data()+endpoint*kLiveFFT,n,liveWaveTables.data()+liveSpareIndex*kLiveFFT);
                liveWaveSize[liveSpareIndex]=n;liveWaveHz[liveSpareIndex]=liveWaveHz[endpoint];
                liveWaveBoundBase[liveSpareIndex]=liveWaveBoundBase[endpoint];
                liveWaveBoundCount[liveSpareIndex]=liveWaveBoundCount[endpoint];
                liveWaveMinHz[liveSpareIndex]=liveWaveMinHz[endpoint];liveWaveMaxHz[liveSpareIndex]=liveWaveMaxHz[endpoint];
                liveWaveBandSum[liveSpareIndex]=liveWaveBandSum[endpoint];liveWaveRms[liveSpareIndex]=liveWaveRms[endpoint];liveWaveRoundoff[liveSpareIndex]=liveWaveRoundoff[endpoint];
                const int base=liveWaveBoundBase[endpoint],length=liveWaveBoundCount[endpoint]-base+1;
                if(length>0)for(int part=0;part<2;++part)
                    std::copy_n(liveWaveBounds.data()+(endpoint*2+part)*(kLiveHarmonics+1)+base,length,
                                liveWaveBounds.data()+(liveSpareIndex*2+part)*(kLiveHarmonics+1)+base);
            }
            else if(liveWaveSize[liveOldIndex]>0 && liveWaveSize[liveOldIndex]==liveWaveSize[liveCurrentIndex]
               && std::abs(liveWaveHz[liveOldIndex]-liveWaveHz[liveCurrentIndex])
                  <= 2e-7*std::max(1.0,liveWaveHz[liveCurrentIndex])) {
                const int n=liveWaveSize[liveOldIndex];
                auto* dst=liveWaveTables.data()+liveSpareIndex*kLiveFFT;
                const auto* aWave=liveWaveTables.data()+liveOldIndex*kLiveFFT;
                const auto* bWave=liveWaveTables.data()+liveCurrentIndex*kLiveFFT;
                for(int i=0;i<n;++i) dst[i]=static_cast<float>(aWave[i]*(1-liveSpectrumBlend)+bWave[i]*liveSpectrumBlend);
                liveWaveSize[liveSpareIndex]=n;
                liveWaveHz[liveSpareIndex]=liveWaveHz[liveOldIndex]*(1-liveSpectrumBlend)+liveWaveHz[liveCurrentIndex]*liveSpectrumBlend;
                buildLiveWaveBounds(liveSpareIndex,count);
            } else liveWaveSize[liveSpareIndex]=0;
            const int free = liveOldIndex;
            liveOldIndex = liveSpareIndex;
            liveSpareIndex = liveCurrentIndex;
            liveCurrentIndex = free;
            liveOldCount = count;
            liveOldSourceCenter =
                liveOldSourceCenter * (1 - liveSpectrumBlend) + liveCurrentSourceCenter * liveSpectrumBlend;
        }
        livePackedFFT (packed);
        const int harmonics = std::min (kLiveHarmonics, packed - 1);
        auto *coefficients = liveCoefficients (liveCurrentIndex);
        const int step = kLiveFFT / size;
        for (int h = 0; h <= harmonics; ++h)
        {
            const int reverse = h == 0 ? 0 : packed - h;
            const double ar = liveReal[static_cast<size_t> (h)], ai = liveImag[static_cast<size_t> (h)];
            const double br = liveReal[static_cast<size_t> (reverse)],
                         bi = liveImag[static_cast<size_t> (reverse)];
            const double c = liveTwiddleReal[static_cast<size_t> (h * step)];
            const double s = liveTwiddleImag[static_cast<size_t> (h * step)];
            coefficients[h] = static_cast<float> (0.5 * ((ar + br) + s * (ar - br) + c * (ai + bi)) / size);
            coefficients[kLiveHarmonics + 1 + h] =
                static_cast<float> (0.5 * ((ai - bi) + s * (ai + bi) - c * (ar - br)) / size);
        }
        // Formant-preserving overlap can cancel partial energy even when all
        // window weights sum to one. Match the known source cycle's energy,
        // bounded by that same cycle's available crest headroom; no output-level
        // threshold, uncorrected pitched blend, or output limiter is involved.
        double sourceGain = 1.0;
        if (energy > 1e-10 && sourceEnergy > 1e-10 && modelPeak > 1e-8)
            sourceGain =
                std::min (std::clamp (std::sqrt (sourceEnergy / energy), 0.5, 1.35), sourcePeak / modelPeak);
        for (int h = 0; h <= harmonics; ++h)
        {
            coefficients[h] *= static_cast<float> (sourceGain);
            coefficients[kLiveHarmonics + 1 + h] *= static_cast<float> (sourceGain);
        }
        double rotation = livePreviousRotation;
        if (std::isfinite (forcedRotation))
            rotation = forcedRotation;
        else if (morph)
        {
            const auto *previous = liveCoefficients (liveOldIndex);
            const int count = std::min ({32, harmonics, liveOldCount});
            std::array<double, 33> crossReal{}, crossImag{};
            for (int h = 1; h <= count; ++h)
            {
                const double re = coefficients[h], im = coefficients[kLiveHarmonics + 1 + h];
                crossReal[h] = re * previous[h] + im * previous[kLiveHarmonics + 1 + h];
                crossImag[h] = re * previous[kLiveHarmonics + 1 + h] - im * previous[h];
            }
            const auto score = [&] (double angle) noexcept
            {
                const double rc = std::cos (angle), rs = std::sin (angle);
                double c = 1, s = 0, result = 0;
                for (int h = 1; h <= count; ++h)
                {
                    double nc = c * rc - s * rs;
                    s = s * rc + c * rs;
                    c = nc;
                    result += crossReal[h] * c + crossImag[h] * s;
                }
                return result;
            };
            constexpr double step = 2.0 * 3.141592653589793 / 128.0;
            int bestOffset = 0;
            double best = score (rotation);
            for (int offset = -16; offset <= 16; ++offset)
            {
                const double value = score (rotation + offset * step);
                if (value > best)
                {
                    best = value;
                    bestOffset = offset;
                }
            }
            double fraction = 0;
            if (std::abs (bestOffset) < 16)
            {
                const double lo = score (rotation + (bestOffset - 1) * step),
                             hi = score (rotation + (bestOffset + 1) * step);
                const double curvature = lo - 2 * best + hi;
                if (curvature < -1e-15)
                    fraction = std::clamp (.5 * (lo - hi) / curvature, -.5, .5);
            }
            rotation += (bestOffset + fraction) * step;
        }
        const double rc = std::cos (rotation), rs = std::sin (rotation);
        double c = 1, s = 0;
        for (int h = 1; h <= harmonics; ++h)
        {
            const double nc = c * rc - s * rs;
            s = s * rc + c * rs;
            c = nc;
            const double re = coefficients[h], im = coefficients[kLiveHarmonics + 1 + h];
            coefficients[h] = static_cast<float> (re * c - im * s);
            coefficients[kLiveHarmonics + 1 + h] = static_cast<float> (re * s + im * c);
        }
        livePreviousRotation = rotation;
        liveCurrentSourceCenter = epoch - 0.5 * period;
        liveModelSourceGain = static_cast<float> (sourceGain);
        if(!liveWaveClockValid || std::abs(liveWaveLastWantedHz-outputHz)>2e-7*std::max(1.0,outputHz)) {
            liveWaveClockValid=true;liveWaveStableSince=inCount;
        }
        liveWaveLastWantedHz=outputHz;
        if(harmonics>=1023 || inCount-liveWaveStableSince>=static_cast<int64_t>(sr*.008)) {
            if(morph && !liveWaveAcceptsFrequency(liveOldIndex,outputHz))
                buildLiveWaveTable(liveOldIndex,liveOldCount,outputHz);
            buildLiveWaveTable(liveCurrentIndex,harmonics,outputHz);
        } else liveWaveSize[liveCurrentIndex]=0;
        liveCurrentCount = harmonics;
        liveSpectrumBlend = morph ? 0.0 : 1.0;
        liveSpectrumValid = true;
        liveModelCoverage = static_cast<float> (coverage / size);
        liveModelContributors = static_cast<int> (std::lround (contributors / size));
        liveModelCancellation = static_cast<float> (cancellation / size);
        liveModelKey = {epoch, period, hop, rate};
        return rotation;
    }
    void buildLiveWaveTable(int bank,int harmonics,double frequency) noexcept
    {
        // Build an oversampled view of the same tapered Fourier coefficients.
        // Shared FFT scratch is used only after source-spectrum extraction ends.
        const double ceiling=std::min(24000.0,.49*sr),roll=std::max(500.0,.05*ceiling);
        const int count=std::min(harmonics,static_cast<int>(ceiling/std::max(1.0,frequency)));
        const int n=nextPow2(std::max(128,8*(count+1)));
        if(n>kLiveFFT) {liveWaveSize[bank]=0;return;}
        const int m=n/2,step=kLiveFFT/n;
        const auto* x=liveCoefficients(bank);
        const auto coefficient=[&](int h,bool imaginary) noexcept {
            if(h>count) return 0.0;
            const double band=h==0 ? 1.0 : std::clamp((ceiling-h*frequency)/roll,0.0,1.0);
            return band*x[(imaginary ? kLiveHarmonics+1 : 0)+h];
        };
        for(int h=0;h<m;++h) {
            const double ar=coefficient(h,false),ai=coefficient(h,true);
            const double br=coefficient(m-h,false),bi=-coefficient(m-h,true);
            const double c=liveTwiddleReal[static_cast<size_t>(h*step)],
                         t=liveTwiddleImag[static_cast<size_t>(h*step)];
            liveReal[h]=static_cast<float>(.5*((ar+br)+t*(ar-br)-c*(ai-bi)));
            // Conjugation lets the existing forward packed complex FFT serve
            // as an unnormalized inverse without an additional scratch buffer.
            liveImag[h]=static_cast<float>(-.5*((ai+bi)+t*(ai-bi)+c*(ar-br)));
        }
        livePackedFFT(m);
        auto* table=liveWaveTables.data()+bank*kLiveFFT;
        for(int i=0;i<m;++i) {table[2*i]=2*liveReal[i];table[2*i+1]=-2*liveImag[i];}
        liveWaveSize[bank]=n;liveWaveHz[bank]=frequency;
        buildLiveWaveBounds(bank,harmonics);
    }
    void buildLiveWaveBounds(int bank,int harmonics) noexcept {
        if(harmonics<1023) {liveWaveBoundCount[bank]=-1;return;}
        auto* prefix=liveWaveBounds.data()+bank*2*(kLiveHarmonics+1);
        auto* weighted=prefix+kLiveHarmonics+1;
        const auto* c=liveCoefficients(bank);
        const double ceiling=std::min(24000.0,.49*sr),roll=std::max(500.0,.05*ceiling);
        const double frequency=liveWaveHz[bank];
        liveWaveMinHz[bank]=frequency*.875;liveWaveMaxHz[bank]=frequency*1.125;
        const int base=static_cast<int>(std::min(static_cast<double>(harmonics),(ceiling-roll)/liveWaveMaxHz[bank]));
        const int count=static_cast<int>(std::min(static_cast<double>(harmonics),ceiling/liveWaveMinHz[bank]));
        // Parseval RMS of this table's nominal, tapered Fourier model makes
        // the additional cutoff-error budget invariant under input gain.
        double power=static_cast<double>(c[0])*c[0];
        for(int h=1;h<=base;++h) {
            const double re=c[h],im=c[kLiveHarmonics+1+h];power+=2*(re*re+im*im);
        }
        prefix[base]=weighted[base]=0;
        for(int h=base+1;h<=count;++h) {
            const double re=c[h],im=c[kLiveHarmonics+1+h];
            const double square=re*re+im*im,magnitude=std::sqrt(square);
            const double band=std::clamp((ceiling-h*frequency)/roll,0.,1.);
            power+=2*square*band*band;
            prefix[h]=prefix[h-1]+magnitude;weighted[h]=weighted[h-1]+h*magnitude;
        }
        liveWaveBoundBase[bank]=base;liveWaveBoundCount[bank]=count;liveWaveRms[bank]=std::sqrt(power);
        liveWaveRoundoff[bank]=1e-10*(liveWaveRms[bank]+prefix[count]);
        liveWaveBandSum[bank]=liveWaveWeightedBandSum(bank,frequency);
    }
    double liveWaveWeightedBandSum(int bank,double frequency) const noexcept {
        const int count=liveWaveBoundCount[bank],base=liveWaveBoundBase[bank];
        const auto* prefix=liveWaveBounds.data()+bank*2*(kLiveHarmonics+1);
        const auto* weighted=prefix+kLiveHarmonics+1;
        const double ceiling=std::min(24000.0,.49*sr),roll=std::max(500.0,.05*ceiling);
        const double inverse=1/frequency;
        const int flat=std::clamp(static_cast<int>(std::min(static_cast<double>(count),(ceiling-roll)*inverse)),base,count);
        const int top=std::clamp(static_cast<int>(std::min(static_cast<double>(count),ceiling*inverse)),base,count);
        return prefix[flat]+(ceiling*(prefix[top]-prefix[flat])-frequency*(weighted[top]-weighted[flat]))/roll;
    }
    bool liveWaveAcceptsFrequency(int bank,double frequency) const noexcept {
        if(liveWaveSize[bank]<=0 || !std::isfinite(frequency) || frequency<=0)return false;
        if(std::abs(liveWaveHz[bank]-frequency)<=2e-7*std::max(1.0,frequency))return true;
        if(liveWaveBoundCount[bank]<0 || frequency<liveWaveMinHz[bank] || frequency>liveWaveMaxHz[bank])return false;
        // Below the cached base, both tapers are exactly one; above the cached
        // top, both are zero throughout this frequency interval. Only the tail
        // contributes to their difference. Monotonic tapers and the triangle
        // inequality bound the waveform error at every phase.
        const double error=2*std::abs(liveWaveWeightedBandSum(bank,frequency)-liveWaveBandSum[bank])+liveWaveRoundoff[bank];
        return error<=2e-6*liveWaveRms[bank];
    }
    bool tryLiveWave(double phase,double frequency,float& result) const noexcept
    {
        const auto valid=[&](int bank) noexcept {return liveWaveAcceptsFrequency(bank,frequency);};
        if(!valid(liveCurrentIndex) || (liveSpectrumBlend<1 && !valid(liveOldIndex)))return false;
        const auto sample=[&](int bank) noexcept {
            const int n=liveWaveSize[bank],mask=n-1;
            const auto* t=liveWaveTables.data()+bank*kLiveFFT;
            const double p=phase*n;const int i=static_cast<int>(p);const double f=p-i;
            const double slot=f*1024;const int index=std::min(1023,static_cast<int>(slot));
            const double fraction=slot-index;
            const auto* a=liveWaveKernel.data()+index*32;
            const auto* b=a+32;
            double value=0;
            for(int k=0;k<32;++k)value+=t[(i+k-15)&mask]*(a[k]+fraction*(b[k]-a[k]));
            return value;
        };
        double value=sample(liveCurrentIndex);
        if(liveSpectrumBlend<1)value=value*liveSpectrumBlend+sample(liveOldIndex)*(1-liveSpectrumBlend);
        result=static_cast<float>(value);return true;
    }
    float evaluateLiveSpectrum(double phase,double frequency) const noexcept
    {
        if(!liveSpectrumValid)return 0;
        float cached=0;
        if(tryLiveWave(phase,frequency,cached))return cached;
        return evaluateLiveSpectrumHorner(phase,frequency);
    }
    float evaluateLiveSpectrumHorner (double phase,double frequency) const noexcept
    {
#if defined(__SSE2__) || defined(_M_X64)
        // P(z) = A0(z^4) + z*A1(z^4) + z^2*A2(z^4) + z^3*A3(z^4).
        // Four independent residue classes avoid the long scalar recurrence;
        // SSE2 evaluates two double-precision classes per lane pair. Every
        // harmonic retains its current morph weight and instantaneous band edge.
        if(!liveSpectrumValid)return 0;
        const auto* old=liveCoefficients(liveOldIndex);
        const auto* current=liveCoefficients(liveCurrentIndex);
        const double c=std::cos(2*3.141592653589793*phase),s=std::sin(2*3.141592653589793*phase);
        const double c2=c*c-s*s,s2=2*c*s,c4=c2*c2-s2*s2,s4=2*c2*s2;
        const double ceiling=std::min(24000.0,.49*sr),roll=std::max(500.0,.05*ceiling);
        const int count=std::min({kLiveHarmonics,std::max(liveOldCount,liveCurrentCount),
                                  static_cast<int>(ceiling/std::max(1.0,frequency))});
        const int flat=std::clamp(static_cast<int>(std::min(static_cast<double>(count),(ceiling-roll)/frequency)),0,count);
        const int top=(count/4)*4;
        const auto coefficient=[&](int h,bool imaginary) noexcept {
            if(h<1 || h>count)return 0.0;
            const int k=h+(imaginary ? kLiveHarmonics+1 : 0);
            const double a=h<=liveOldCount ? old[k] : 0.0,b=h<=liveCurrentCount ? current[k] : 0.0;
            const double band=std::clamp((ceiling-h*frequency)/roll,0.0,1.0);
            return band*(a*(1-liveSpectrumBlend)+b*liveSpectrumBlend);
        };
        __m128d ar=_mm_set_pd(coefficient(top+1,false),coefficient(top,false));
        __m128d ai=_mm_set_pd(coefficient(top+1,true),coefficient(top,true));
        __m128d br=_mm_set_pd(coefficient(top+3,false),coefficient(top+2,false));
        __m128d bi=_mm_set_pd(coefficient(top+3,true),coefficient(top+2,true));
        const __m128d cr=_mm_set1_pd(c4),ci=_mm_set1_pd(s4),
            wa=_mm_set1_pd(1-liveSpectrumBlend),wb=_mm_set1_pd(liveSpectrumBlend),
            f=_mm_set1_pd(frequency),limit=_mm_set1_pd(ceiling),width=_mm_set1_pd(roll),
            zero=_mm_setzero_pd(),one=_mm_set1_pd(1.0);
        const auto evaluate=[&]<bool Both>() noexcept {
            const auto group=[&]<bool Taper>(int base) noexcept {
                const __m128 hr0=_mm_loadu_ps(old+base),hi0=_mm_loadu_ps(old+kLiveHarmonics+1+base);
                const __m128 hr1=_mm_loadu_ps(current+base),hi1=_mm_loadu_ps(current+kLiveHarmonics+1+base);
                __m128d r0=_mm_cvtps_pd(hr0),i0=_mm_cvtps_pd(hi0),
                        r1=_mm_cvtps_pd(_mm_movehl_ps(hr0,hr0)),i1=_mm_cvtps_pd(_mm_movehl_ps(hi0,hi0)),
                        r2=_mm_cvtps_pd(hr1),i2=_mm_cvtps_pd(hi1),
                        r3=_mm_cvtps_pd(_mm_movehl_ps(hr1,hr1)),i3=_mm_cvtps_pd(_mm_movehl_ps(hi1,hi1));
                const __m128d h0=_mm_set_pd(base+1,base),h1=_mm_set_pd(base+3,base+2);
                if constexpr(!Both) {
                    const __m128d oa=_mm_set1_pd(liveOldCount),ob=_mm_set1_pd(liveCurrentCount);
                    const __m128d m0=_mm_cmple_pd(h0,oa),m1=_mm_cmple_pd(h1,oa),
                                  m2=_mm_cmple_pd(h0,ob),m3=_mm_cmple_pd(h1,ob);
                    r0=_mm_and_pd(r0,m0);i0=_mm_and_pd(i0,m0);r1=_mm_and_pd(r1,m1);i1=_mm_and_pd(i1,m1);
                    r2=_mm_and_pd(r2,m2);i2=_mm_and_pd(i2,m2);r3=_mm_and_pd(r3,m3);i3=_mm_and_pd(i3,m3);
                }
                r0=_mm_add_pd(_mm_mul_pd(r0,wa),_mm_mul_pd(r2,wb));
                i0=_mm_add_pd(_mm_mul_pd(i0,wa),_mm_mul_pd(i2,wb));
                r1=_mm_add_pd(_mm_mul_pd(r1,wa),_mm_mul_pd(r3,wb));
                i1=_mm_add_pd(_mm_mul_pd(i1,wa),_mm_mul_pd(i3,wb));
                if constexpr(Taper) {
                    const __m128d w0=_mm_min_pd(one,_mm_max_pd(zero,_mm_div_pd(_mm_sub_pd(limit,_mm_mul_pd(h0,f)),width)));
                    const __m128d w1=_mm_min_pd(one,_mm_max_pd(zero,_mm_div_pd(_mm_sub_pd(limit,_mm_mul_pd(h1,f)),width)));
                    r0=_mm_mul_pd(r0,w0);i0=_mm_mul_pd(i0,w0);r1=_mm_mul_pd(r1,w1);i1=_mm_mul_pd(i1,w1);
                }
                if(base==0) {
                    const __m128d mask=_mm_castsi128_pd(_mm_set_epi32(-1,-1,0,0));
                    r0=_mm_and_pd(r0,mask);i0=_mm_and_pd(i0,mask);
                }
                const __m128d nr0=_mm_add_pd(r0,_mm_sub_pd(_mm_mul_pd(cr,ar),_mm_mul_pd(ci,ai)));
                const __m128d ni0=_mm_add_pd(i0,_mm_add_pd(_mm_mul_pd(cr,ai),_mm_mul_pd(ci,ar)));
                const __m128d nr1=_mm_add_pd(r1,_mm_sub_pd(_mm_mul_pd(cr,br),_mm_mul_pd(ci,bi)));
                const __m128d ni1=_mm_add_pd(i1,_mm_add_pd(_mm_mul_pd(cr,bi),_mm_mul_pd(ci,br)));
                ar=nr0;ai=ni0;br=nr1;bi=ni1;
            };
            int base=top-4;
            for(;base>=0 && base+3>flat;base-=4)group.template operator()<true>(base);
            for(;base>=0;base-=4)group.template operator()<false>(base);
        };
        if(std::min(liveOldCount,liveCurrentCount)>=count)evaluate.template operator()<true>();
        else evaluate.template operator()<false>();
        const double c3=c2*c-s2*s,s3=s2*c+c2*s;
        __m128d sum=_mm_add_pd(_mm_sub_pd(_mm_mul_pd(ar,_mm_set_pd(c,1)),_mm_mul_pd(ai,_mm_set_pd(s,0))),
                              _mm_sub_pd(_mm_mul_pd(br,_mm_set_pd(c3,c2)),_mm_mul_pd(bi,_mm_set_pd(s3,s2))));
        const double dc=(liveOldCount>=0 ? old[0]*(1-liveSpectrumBlend) : 0.0)+current[0]*liveSpectrumBlend;
        return static_cast<float>(dc+2*(_mm_cvtsd_f64(sum)+_mm_cvtsd_f64(_mm_unpackhi_pd(sum,sum))));
#else
        return evaluateLiveSpectrumDirect(phase,frequency);
#endif
    }
    float evaluateLiveSpectrumDirect (double phase, double frequency) const noexcept
    {
        if (!liveSpectrumValid)
            return 0.0f;
        const auto *old = liveCoefficients (liveOldIndex);
        const auto *current = liveCoefficients (liveCurrentIndex);
        const double c = std::cos (2.0 * 3.141592653589793 * phase),
                     s = std::sin (2.0 * 3.141592653589793 * phase);
        const double ceiling = std::min (24000.0, 0.49 * sr), roll = std::max (500.0, 0.05 * ceiling);
        const int count = std::min ({kLiveHarmonics, std::max (liveOldCount, liveCurrentCount),
                                     static_cast<int> (ceiling / std::max (1.0, frequency))});
        double a1 = 0, a2 = 0, b1 = 0, b2 = 0;
        for (int h = count; h >= 1; --h)
        {
            const double ar = h <= liveOldCount ? old[h] : 0.0;
            const double ai = h <= liveOldCount ? old[kLiveHarmonics + 1 + h] : 0.0;
            const double br = h <= liveCurrentCount ? current[h] : 0.0;
            const double bi = h <= liveCurrentCount ? current[kLiveHarmonics + 1 + h] : 0.0;
            const double band = std::clamp ((ceiling - h * frequency) / roll, 0.0, 1.0);
            const double re = band * (ar * (1 - liveSpectrumBlend) + br * liveSpectrumBlend);
            const double im = band * (ai * (1 - liveSpectrumBlend) + bi * liveSpectrumBlend);
            const double a = re + 2 * c * a1 - a2, b = im + 2 * c * b1 - b2;
            a2 = a1;
            a1 = a;
            b2 = b1;
            b1 = b;
        }
        const double dc =
            (liveOldCount >= 0 ? old[0] * (1 - liveSpectrumBlend) : 0.0) + current[0] * liveSpectrumBlend;
        return static_cast<float> (dc + 2 * (c * a1 - a2 - s * b1));
    }
    float processLiveSpectrumSample (float input, double prior, double ratio) noexcept
    {
        inRing[static_cast<size_t> (inCount & inMask)] = input;
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
        if(!sharedMarks || phaseLeader)epochWhitening.feed(input,inCount);
#endif
        ++inCount;
        const double wantedHop = prior / ratio, wantedHz = sr / wantedHop;
        const bool fullyDry = dryMixTarget > 0.5f && dryMix > 0.999f;
        if (fullyDry)
        {
            liveSpectrumValid = false;
            livePhaseInitialized = false;
            livePreviousRotation = 0;
        }
        if (!sharedMarks || phaseLeader)
        {
            if (!liveSpectrumValid && dryMix > 0.995f && recordedEpochValid &&
                (recordedPeriod / prior < 0.75 || recordedPeriod / prior > 1.33))
                recordedEpochValid = false;
            updateRecordedEpoch (prior, true);
            if (recordedEpochValid && !fullyDry)
            {
                const bool sourceChanged = recordedEpoch != liveLastSourceEpoch;
                const bool parametersChanged =
                    std::abs (smoothedFormantPreserve - liveLastPreserve) > 1e-5 ||
                    std::abs (smoothedFormantShiftRatio - liveLastColour) > 1e-5 ||
                    (smoothedFormantPreserve > 1e-6 && std::abs (wantedHop - liveLastHop) > 1e-5);
                // A source-cycle update can otherwise follow a control rebuild
                // immediately and repeat the largest (2048+ sample) model work.
                // Keep its latest source pending for the existing 2-ms control
                // interval. First models and smaller banks still update at once;
                // the output phase clock continues on every sample.
                const bool buildReady = inCount - liveLastBuild >= liveControlSamples;
                const bool sourceReady = liveCurrentCount < 1023 || buildReady;
                if (!liveSpectrumValid || (sourceChanged && sourceReady) ||
                    (parametersChanged && buildReady))
                {
                    const double effectiveRatio = recordedPeriod / wantedHop;
                    const double preserve =
                        smoothedFormantPreserve == 1.0 ? 1.0
                        : smoothedFormantPreserve == 0.0
                            ? effectiveRatio
                            : std::exp ((1.0 - smoothedFormantPreserve) * std::log (effectiveRatio));
                    const double rate = preserve * smoothedFormantShiftRatio;
                    const double rotation =
                        buildLiveSpectrum (recordedEpoch, recordedPeriod, wantedHop, rate);
                    if (sharedMarks)
                    {
                        auto &e = sharedMarks->history[emittedGrains % PhaseMarkLink::capacity];
                        e = {emittedGrains, recordedEpoch, recordedPeriod, inCount, ringEpoch,
                             recordedEpoch, rotation,      wantedHop,      rate,    recordedEpochConfidence};
                    }
                    ++emittedGrains;
                    liveLastSourceEpoch = recordedEpoch;
                    liveLastPreserve = smoothedFormantPreserve;
                    liveLastColour = smoothedFormantShiftRatio;
                    liveLastHop = wantedHop;
                    liveLastBuild = inCount;
                }
            }
        }
        else if (!fullyDry)
        {
            const auto &e = sharedMarks->history[emittedGrains % PhaseMarkLink::capacity];
            if (e.grain == emittedGrains && e.epoch == ringEpoch && e.when <= inCount)
            {
                recordedEpoch = e.mark;
                recordedPeriod = e.period;
                recordedEpochConfidence = e.match;
                recordedEpochValid = true;
                buildLiveSpectrum (e.mark, e.period, e.hop, e.rate, e.phase);
                ++emittedGrains;
            }
        }
        if (liveSpectrumValid && !livePhaseInitialized)
        {
            livePhase = (static_cast<double> (outRead - latency) - recordedEpoch) / recordedPeriod;
            livePhase -= std::floor (livePhase);
            liveFrequency = wantedHz;
            livePhaseInitialized = true;
        }
        liveFrequency = wantedHz + (liveFrequency - wantedHz) * liveFrequencyCoefficient;
        const float wet = evaluateLiveSpectrum (livePhase, liveFrequency);
        livePhase += liveFrequency / sr;
        livePhase -= std::floor (livePhase);
        liveSpectrumBlend = std::min (1.0, liveSpectrumBlend + liveBlendStep);
        const float coefficient = dryMixTarget > dryMix ? dryReleaseCoeff : dryAttackCoeff;
        dryMix = dryMixTarget + (dryMix - dryMixTarget) * coefficient;
        if (std::abs (dryMix - dryMixTarget) < 1e-6f)
            dryMix = dryMixTarget;
        const int64_t time = outRead - latency;
        const float dry = time >= 0 && time < inCount ? inRing[static_cast<size_t> (time & inMask)] : 0;
        // Bound only handoff compensation using the available crest
        // over a causal pitch cycle, rather than wave-shaping every zero crossing.
        const float instantCrest = std::max(std::abs(wet), std::abs(dry));
        if (instantCrest >= handoffCrest) {
            handoffCrest = instantCrest;
            handoffCrestHold = static_cast<int>(std::ceil(std::clamp(
                std::max(prior, sr / std::max(40.0, liveFrequency)), 8.0, sr * 0.025)));
        } else if (handoffCrestHold > 0) {
            --handoffCrestHold;
        } else {
            handoffCrest = std::max(instantCrest, handoffCrest * crossfadeGainCoeff);
        }
        float result = wet * (1 - dryMix) + dry * dryMix;
        if (energyMatchedHandoff && dryMix > 0.005f && dryMix < 0.995f)
        {
            const float ec = crossfadeEnergyCoeff, a = 1 - dryMix, b = dryMix;
            crossfadeWetPower = ec * crossfadeWetPower + (1 - ec) * wet * wet;
            crossfadeDryPower = ec * crossfadeDryPower + (1 - ec) * dry * dry;
            crossfadeCov = ec * crossfadeCov + (1 - ec) * wet * dry;
            const float desired = a * crossfadeWetPower + b * crossfadeDryPower;
            const float actual =
                a * a * crossfadeWetPower + b * b * crossfadeDryPower + 2 * a * b * crossfadeCov;
            const float gain = desired > 1e-7f && actual > 1e-7f
                                   ? std::clamp (std::sqrt (desired / actual), 1.0f, 1.35f)
                                   : 1.0f;
            const float shaped = 1 + 4 * a * b * (gain - 1);
            crossfadeGain = shaped + crossfadeGainCoeff * (crossfadeGain - shaped);
            const float headroom =
                handoffCrest / std::max (std::abs (result), 1e-20f);
            result *= std::min (crossfadeGain, headroom);
        }
        else
        {
            crossfadeGain = 1;
            crossfadeWetPower = crossfadeDryPower = crossfadeCov = 0;
        }
        grainDiagnostics.maxOutputStep =
            std::max (grainDiagnostics.maxOutputStep, std::abs (result - lastOutput));
        grainDiagnostics.phaseMatchScore = recordedEpochConfidence;
        lastOutput = result;
        diagnosticCoverageSum += liveSpectrumValid ? liveModelCoverage : 0;
        diagnosticGainSum += liveSpectrumValid ? liveModelSourceGain : 1.0f;
        diagnosticContributorsSum += liveModelContributors;
        ++diagnosticCount;
        if (detailedDiagnostics)
        {
            diagnosticCancellationSum += liveModelCancellation;
            ++diagnosticCancellationCount;
        }
        ++outRead;
        return result;
    }

    double standardPreserveFactor(double ratio) noexcept
    {
        // The sample scheduler revisits unchanged controls many times. Cache only
        // pure control math; both inputs are keys so automation cannot reuse stale colour.
        if (ratio != cachedPreserveRatio || smoothedFormantPreserve != cachedPreserveAmount)
        {
            cachedPreserveRatio = ratio;
            cachedPreserveAmount = smoothedFormantPreserve;
            cachedPreserveFactor = smoothedFormantPreserve == 1.0 ? 1.0
                : (smoothedFormantPreserve == 0.0 ? ratio
                   : std::exp((1.0 - smoothedFormantPreserve) * std::log(ratio)));
        }
        return cachedPreserveFactor;
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
        const double preserveFactor = standardPreserveFactor(ratio);
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
                const double precedingMark = markPos;
                markPos += periodF;
                if (!lowLatencyExperiment && sourceEpochTracking)
                    markPos = correlateSourceEpoch(markPos, precedingMark, periodF);
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

            // The source-channel peak search is extremely sensitive to tiny
            // channel quantization differences. Without linking, a dual-mono
            // recording can produce opposing grains in L/R and partially
            // cancel in mono. Apply this only to the experimental scheduler:
            // Standard's established stereo processing remains unchanged.
            if (lowLatencyExperiment && sharedMarks)
            {
                auto& entry = sharedMarks->history[emittedGrains % PhaseMarkLink::capacity];
                if (phaseLeader)
                    entry = {emittedGrains, markPos};
                else if (entry.grain == emittedGrains)
                    markPos = entry.mark;
            }
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
                {
                    outWeight[outputIndex] += w * gain;
                    if (outContributors[outputIndex] < 255) ++outContributors[outputIndex];
                }
            }

            nextTs += hop;
            ++emittedGrains;
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
                    float gainCorrection = 1.0f / std::clamp(coverage, 0.25f, 1.50f);
                    if (!lowLatencyExperiment)
                    {
                        // With formants off, window width equals output hop:
                        // coverage should sum to unity. Correct deviations at
                        // target changes. Formant-preserving grains deliberately
                        // use different geometry; retain their established gain.
                        gainCorrection = 1.0f + static_cast<float>(1.0 - smoothedFormantPreserve)
                            * (gainCorrection - 1.0f);
                    }
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
                if (energyMatchedHandoff && dryMix > 0.005f && dryMix < 0.995f)
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
    double periodSliceCoefficient = 0.0, colourSliceCoefficient = 0.0, preserveSliceCoefficient = 0.0;
    double cachedPreserveRatio = 1.0, cachedPreserveAmount = 1.0, cachedPreserveFactor = 1.0;
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
    PhaseMarkLink* sharedMarks = nullptr;
    bool phaseLeader = true;
    uint64_t emittedGrains = 0;
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
    float handoffCrest = 0;
    int handoffCrestHold = 0;
    bool energyMatchedHandoff = true;
    bool    lowLatencyExperiment = false;
    bool phaseLockEnabled = false;
    bool sourceEpochTracking = true;
    int epochRadiusLimit = 48, epochSpanLimit = 512, epochSampleStride = 3;
    std::vector<float> liveCoefficientData, liveReal, liveImag, liveTwiddleReal, liveTwiddleImag, liveHann;
    bool liveWaveClockValid=false;
    int64_t liveWaveStableSince=0;
    double liveWaveLastWantedHz=0;
    std::vector<uint16_t> liveBitReversal;
    std::vector<float> liveStageReal,liveStageImag;
    std::vector<float> liveWaveKernel;
    std::vector<float> liveWaveTables;
    std::vector<double> liveWaveBounds;
    std::vector<double> epochReference;
    std::array<int,3> liveWaveBoundCount{},liveWaveBoundBase{};
    std::array<double,3> liveWaveMinHz{},liveWaveMaxHz{};
    std::array<double,3> liveWaveBandSum{},liveWaveRms{},liveWaveRoundoff{};
    std::array<int,3> liveWaveSize{};
    std::array<double,3> liveWaveHz{};
    bool liveBackendActive = false, liveSpectrumValid = false, livePhaseInitialized = false;
    int liveCurrentIndex = 0, liveOldIndex = 1, liveSpareIndex = 2, liveCurrentCount = 0, liveOldCount = 0,
        liveControlSamples = 96;
    int liveModelContributors = 0;
    double liveSpectrumBlend = 1, liveBlendStep = 0, livePhase = 0, liveFrequency = 200,
           liveFrequencyCoefficient = 0, liveLowFrequencyCoefficient = 0, liveStandardFrequencyCoefficient = 0;
    double liveOldSourceCenter = 0, liveCurrentSourceCenter = 0;
    double livePreviousRotation = 0, liveLastSourceEpoch = -1e12, liveLastPreserve = -1, liveLastColour = -1,
           liveLastHop = -1;
    int64_t liveLastBuild = -1000000;
    struct LiveModelKey { double epoch = 0, period = 0, hop = 0, rate = 0; };
    LiveModelKey liveModelKey;
    float liveModelCoverage = 0, liveModelCancellation = 0, liveModelSourceGain = 1;
#if SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
    EpochWhitening epochWhitening;
    EpochObservation epochObservation;
#endif
    bool recordedEpochValid = false;
    double recordedEpoch = 0.0, recordedPeriod = 0.0;
    float candidateEpochConfidence = 0.0f, recordedEpochConfidence = 0.0f;
    double  lowLatencyBudgetMs = 3.5;
    double  formantShiftRatioTarget = 1.0;
    double  smoothedFormantShiftRatio = 1.0;
};

} // namespace humtune
