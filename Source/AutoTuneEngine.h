#pragma once

#include "PitchDetector.h"
#include "PitchShifter.h"
#include "ScaleQuantizer.h"
#include "KeyEstimator.h"
#include "TargetGlide.h"
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace humtune
{

class AutoTuneEngine
{
public:
    static constexpr std::array<bool, 12> kMajor     = {true,false,true,false,true,true,false,true,false,true,false,true};
    static constexpr std::array<bool, 12> kMinor     = {true,false,true,true,false,true,false,true,true,false,true,false};
    static constexpr std::array<bool, 12> kChromatic = {true,true,true,true,true,true,true,true,true,true,true,true};

    static constexpr int kMaxChannels = 2;

    // Voice range = the lowest note the tuner follows. Standard-mode delay is about 1.6
    // periods of that note plus an 8-sample guard, so a higher floor means less delay.
    // The legacy first five parameter indices are preserved for saved-session compatibility;
    // the new Sub Bass option is appended as index 5 rather than shifting old values.
    //    40 Hz -> 40.0 ms   70 Hz -> 23.0 ms   80 Hz -> 20.2 ms
    //   100 Hz -> 16.2 ms  130 Hz -> 12.5 ms  180 Hz ->  9.1 ms
    static constexpr int   kNumRanges    = 6;
    static constexpr float kRangeMinHz[kNumRanges] = { 70.0f, 80.0f, 100.0f, 130.0f, 180.0f, 40.0f };
    static constexpr int   kDefaultRange = 1;
    static constexpr double kExperimentalLowLatencyMs = 1.9;

    void prepare (double sampleRate, int blockSize)
    {
        sr = sampleRate;
        detector.prepare(sampleRate, blockSize);
        // Preallocate detector downmix storage off the audio thread. A generous
        // cap handles oversized host blocks without allocating during process().
        detectorMono.assign(static_cast<size_t>(std::max(8192, blockSize)), 0.0f);
        for (auto& s : shifters)
            s.prepare(sampleRate, blockSize);

        const int maxLatency = static_cast<int>(std::ceil(2.5 * std::ceil(sr / PitchDetector::kMinHz))) + 16;
        for (auto& line : dryLine)
            line.reserve(static_cast<size_t>(maxLatency));

        prepared = true;
        keyEstimator.reset();
        keyEstimateSampleCounter = 0;
        publishKeyEstimate();
        targetGlide.reset();
        applyLatencyMode();
    }

    int   getLatencySamples() const { return latencySamples; }
    float getMinFrequency()   const { return detector.getMinFrequency(); }

    void setRootNote        (int note)    { rootNote = note % 12; }
    void setScaleType       (int type)    { scaleType = ScaleQuantizer::clampType(type); }
    void setRetuneSpeed     (float spd)   { retuneSpeed = spd; }
    void setAmount          (float amt)   { amount = amt; }
    void setHumanize        (float h)     { humanize = std::clamp(h, 0.0f, 1.0f); }
    void setFlexTune        (float f)     { flexTune = std::clamp(std::isfinite(f) ? f : 0.0f, 0.0f, 1.0f); }
    void setGlideMilliseconds (float ms)  { glideMilliseconds = std::clamp(std::isfinite(ms) ? ms : 0.0f, 0.0f, 500.0f); }

    // Flex-Tune keeps full correction when disabled. As the control rises, correction
    // tapers away for larger pitch excursions so scoops/bends can pass through, while
    // small errors near the selected note still receive correction. This is an
    // independently implemented response curve; it does not copy FreeTune's DSP code.
    static float flexCorrectionWeight (float absoluteErrorCents, float flex) noexcept
    {
        flex = std::clamp(std::isfinite(flex) ? flex : 0.0f, 0.0f, 1.0f);
        if (flex <= 0.0f)
            return 1.0f; // exact legacy behavior when the new parameter is at its default

        const float error = std::max(0.0f, std::isfinite(absoluteErrorCents) ? absoluteErrorCents : 0.0f);
        const float fullCorrectionRadius = 6.0f + 42.0f * (1.0f - flex);
        const float fadeWidth = 12.0f + 18.0f * (1.0f - flex);
        const float x = std::clamp((error - fullCorrectionRadius) / fadeWidth, 0.0f, 1.0f);
        const float smooth = x * x * (3.0f - 2.0f * x);
        return std::clamp(1.0f - flex * smooth, 0.0f, 1.0f);
    }
    void setSustain         (float s)     { sustainCents = s; }
    void setNoteStabilizer  (bool on)     { noteStabilizer = on; }
    void setFormantPreserve (bool on)     { formantPreserve = on; }
    void setFormantShiftSemitones (float semitones)
    {
        formantShiftSemitones = std::isfinite(semitones) ? std::clamp(semitones, -4.0f, 4.0f) : 0.0f;
    }
    void setReferenceFreq   (float hz)    { referenceFreq = hz; }
    void setEnabled (bool on)
    {
        if (enabled == on)
            return;

        enabled = on;
        smoothedCorrectionCents = 0.0;
        targetGlide.reset();
        if (on)
        {
            keyEstimator.reset();
            keyEstimateSampleCounter = 0;
            publishKeyEstimate();
        }
        smoothedHumanize = static_cast<double>(std::clamp(humanize, 0.0f, 1.0f));
        smoothedFlexTune = static_cast<double>(std::clamp(flexTune, 0.0f, 1.0f));
        lockedMidiNote = -1;
        holdCounter = 0;
        resetPitchMotionTracker();
        samplesSinceHop = 0;
        unvoicedHopCount = 0;
        lastVoicedPeriodSamples = 0.0;
        curRatio = 1.0f;
        curPeriod = 0.0f;
        lastDetectedHz = lastTargetHz = lastCorrectionCents = 0.0f;
        lastDetectedMidi = lastTargetMidi = -1;
        lastTargetMidiFloat = -1.0f;

        // The shifter is not fed while tuning is bypassed. Clear its stale ring and
        // detector history when re-enabling so it cannot resume with a time gap.
        if (prepared && on)
        {
            detector.setMinFrequency(kRangeMinHz[rangeIndex]);
            for (auto& s : shifters)
                s.reset();
        }
    }

    // Experimental fixed-delay resynthesis. The existing period-bound path remains
    // the default; when enabled, the shifters use a fixed 1.9 ms processing budget.
    // This budget is not a claim about end-to-end DAW latency.
    void setLowLatencyMode (bool on)
    {
        if (lowLatencyMode == on)
            return;

        lowLatencyMode = on;
        for (auto& s : shifters)
        {
            s.setLowLatencyExperiment(on);
            if (on)
                s.setLowLatencyBudgetMilliseconds(kExperimentalLowLatencyMs);
        }

        if (prepared)
            applyLatencyMode();
    }

    bool getLowLatencyMode() const { return lowLatencyMode; }

    // Chooses the voice range (index into kRangeMinHz). Safe to call from the audio thread
    // (no allocation). The latency changes, so the host must be told: see getLatencySamples().
    void setVoiceRange      (int index)
    {
        index = std::max(0, std::min(kNumRanges - 1, index));
        if (index == rangeIndex)
            return;
        rangeIndex = index;
        if (prepared)
            applyLatencyMode();
    }

    void setCustomScale     (const std::array<bool, 12>& s) { customScale = s; useCustom = true; }
    void clearCustomScale   ()            { useCustom = false; }

    float getDetectedHz()       const { return lastDetectedHz; }
    float getAppliedPitchRatio() const { return curRatio; } // diagnostic: ratio currently applied by the shifter
    float getTargetHz()         const { return lastTargetHz; }
    float getCorrectionCents()  const { return lastCorrectionCents; }
    float getConfidence()       const { return detector.getConfidence(); }
    int   getDetectedMidiNote() const { return lastDetectedMidi; }
    int   getTargetMidiNote()   const { return lastTargetMidi; } // nearest piano-key label
    float getTargetMidiFloat()  const { return lastTargetMidiFloat; } // exact applied target, including fractional/microtonal glide

    KeyEstimator::Estimate getAutoKeyEstimate() const noexcept
    {
        KeyEstimator::Estimate e;
        e.root = estimatedKeyRoot.load(std::memory_order_relaxed);
        e.minor = estimatedKeyMinor.load(std::memory_order_relaxed) != 0;
        e.confidence = estimatedKeyConfidence.load(std::memory_order_relaxed);
        e.evidenceSeconds = estimatedKeyEvidence.load(std::memory_order_relaxed);
        e.profileScore = estimatedKeyScore.load(std::memory_order_relaxed);
        e.scoreMargin = estimatedKeyMargin.load(std::memory_order_relaxed);
        e.distinctPitchClasses = estimatedKeyDistinct.load(std::memory_order_relaxed);
        e.valid = estimatedKeyValid.load(std::memory_order_relaxed) != 0;
        return e;
    }

    void delayDry (juce::AudioBuffer<float>& buffer)
    {
        const int n   = buffer.getNumSamples();
        const int nch = std::min(buffer.getNumChannels(), kMaxChannels);
        const int len = latencySamples;
        if (n <= 0 || nch <= 0 || len <= 0)
            return;

        int endPos = dryPos;
        for (int ch = 0; ch < nch; ++ch)
        {
            float* d = buffer.getWritePointer(ch);
            auto& line = dryLine[static_cast<size_t>(ch)];
            int p = dryPos;
            for (int i = 0; i < n; ++i)
            {
                const float x = d[i];
                d[i] = line[static_cast<size_t>(p)];
                line[static_cast<size_t>(p)] = x;
                if (++p >= len)
                    p = 0;
            }
            endPos = p;
        }
        dryPos = endPos;
    }

    void process (juce::AudioBuffer<float>& buffer)
    {
        const int numSamples  = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        if (numChannels == 0 || numSamples == 0 || sr <= 0.0)
            return;

        const int nch = std::min(numChannels, kMaxChannels);

        // When disabled, just pass audio through (with the same delay)
        if (!enabled)
        {
            lastDetectedHz = 0.0f;
            lastTargetHz = 0.0f;
            lastCorrectionCents = 0.0f;
            lastDetectedMidi = -1;
            lastTargetMidi = -1;
            lastTargetMidiFloat = -1.0f;
            smoothedCorrectionCents = 0.0;
            lockedMidiNote = -1;
            holdCounter = 0;
            curRatio = 1.0f;
            curPeriod = 0.0f;

            // True delayed dry passthrough: do not run audio through the shifter at unity
            // ratio, which can still reshape transients and phase. The dry delay line is
            // kept warm during enabled processing so this transition remains aligned.
            delayDry(buffer);
            return;
        }

        // Save the original input to the dry ring without altering the wet buffer. If
        // the user disables tuning later, delayDry() can switch to exact latency-matched
        // passthrough without allocating a second AudioBuffer on the audio thread.
        storeDryInput(buffer);

        // The correction is refreshed every `hop` samples (about 4 ms) instead of once
        // per DAW buffer, and each piece of audio is shifted straight after the pitch
        // of that piece was measured. That keeps the correction in step with the voice.
        const int hop = detector.getHopSize();
        const float* left = buffer.getReadPointer(0);
        const float* right = (nch > 1) ? buffer.getReadPointer(1) : nullptr;

        int pos = 0;
        while (pos < numSamples)
        {
            // Keep the scratch write bounded and allocation-free even if a host
            // supplies an unusually large block.
            const int seg = std::min({ numSamples - pos,
                                       hop - samplesSinceHop,
                                       static_cast<int>(detectorMono.size()) });
            if (right != nullptr)
            {
                // First measure the stereo block. Choosing the stronger sample
                // independently for each sample creates a rectifier-like waveform
                // when the channels are anti-phase. Instead, decide once per segment
                // whether the normal mono sum is badly cancelled, then use one whole
                // channel for the detector. The audible stereo paths remain untouched.
                double leftEnergy = 0.0, rightEnergy = 0.0, sumEnergy = 0.0;
                for (int i = 0; i < seg; ++i)
                {
                    const double l = left[pos + i];
                    const double r = right[pos + i];
                    const double sum = 0.5 * (l + r);
                    leftEnergy += l * l;
                    rightEnergy += r * r;
                    sumEnergy += sum * sum;
                }
                const double channelEnergy = std::max(leftEnergy, rightEnergy);
                const bool downmixCancels = channelEnergy > 1.0e-12
                    && sumEnergy < 0.08 * channelEnergy;
                const bool useRight = rightEnergy > leftEnergy;
                for (int i = 0; i < seg; ++i)
                {
                    const float l = left[pos + i];
                    const float r = right[pos + i];
                    detectorMono[static_cast<size_t>(i)] = downmixCancels
                        ? (useRight ? r : l) : 0.5f * (l + r);
                }
            }
            else
                std::copy(left + pos, left + pos + seg, detectorMono.begin());

            detector.feedSamples(detectorMono.data(), seg);
            samplesSinceHop += seg;
            const bool correctionHopComplete = samplesSinceHop >= hop;

            // Process this segment with the correction state that was active when the
            // segment began. Updating correction before shifting used the just-completed
            // detector window on the *same* samples that created it; the retroactive
            // segment length then depended on the host block size (e.g. 64 vs 256 samples
            // could apply a new ratio to different portions of the same 192-sample hop).
            // Commit the new estimate only after this segment has been processed, so the
            // pitch/ratio timeline is independent of host callback boundaries.
            for (int ch = 0; ch < nch; ++ch)
            {
                auto& s = shifters[static_cast<size_t>(ch)];
                s.setFormantPreserve(formantPreserve);
                s.setFormantShiftSemitones(formantShiftSemitones);
                s.process(buffer.getWritePointer(ch) + pos, seg, curPeriod, curRatio);
            }

            if (correctionHopComplete)
            {
                samplesSinceHop -= hop;
                updateCorrection(hop);
            }

            pos += seg;
        }
    }

private:
    void resetPitchMotionTracker() noexcept
    {
        pitchMotionHasPrevious = false;
        previousObservedMidi = 0.0f;
    }

    float predictPitchMotion (float observedMidi, float observedHz, float confidence, int hop) noexcept
    {
        if (!std::isfinite(observedMidi) || !std::isfinite(observedHz)
            || observedHz <= 0.0f || !std::isfinite(confidence) || confidence < 0.55f
            || hop <= 0 || sr <= 0.0)
        {
            resetPitchMotionTracker();
            return observedMidi;
        }

        if (!pitchMotionHasPrevious)
        {
            previousObservedMidi = observedMidi;
            pitchMotionHasPrevious = true;
            return observedMidi;
        }

        const double deltaCents = (static_cast<double>(observedMidi)
                                   - static_cast<double>(previousObservedMidi)) * 100.0;
        previousObservedMidi = observedMidi;

        // Do not extrapolate a discrete note jump or likely octave correction.
        // A 1.75-semitone frame-to-frame step is well outside normal vibrato and
        // short glides at this hop rate; seed the tracker again after the jump.
        if (std::abs(deltaCents) > 175.0)
        {
            return observedMidi;
        }

        const double dt = static_cast<double>(hop) / sr;
        if (dt <= 0.0)
            return observedMidi;

        const double measuredVelocity = std::clamp(deltaCents / dt, -6000.0, 6000.0);
        // Ignore detector jitter on held notes. A real expressive glide typically
        // exceeds this threshold over a single 4 ms analysis hop. Confidence scales
        // the forecast smoothly instead of adding another hard voicing gate.
        if (std::abs(deltaCents) < 1.5 || std::abs(measuredVelocity) < 350.0)
            return observedMidi;

        const double confidenceWeight = std::clamp((static_cast<double>(confidence) - 0.55) / 0.35,
                                                   0.0, 1.0);
        const double estimateAge = detector.getPitchEstimateAgeSeconds(observedHz);
        const double predictionCents = std::clamp(
            measuredVelocity * estimateAge * confidenceWeight, -85.0, 85.0);
        return observedMidi + static_cast<float>(predictionCents / 100.0);
    }

    void storeDryInput (const juce::AudioBuffer<float>& buffer)
    {
        const int n = buffer.getNumSamples();
        const int nch = std::min(buffer.getNumChannels(), kMaxChannels);
        const int len = latencySamples;
        if (n <= 0 || nch <= 0 || len <= 0)
            return;

        int endPos = dryPos;
        for (int ch = 0; ch < nch; ++ch)
        {
            const float* input = buffer.getReadPointer(ch);
            auto& line = dryLine[static_cast<size_t>(ch)];
            int p = dryPos;
            for (int i = 0; i < n; ++i)
            {
                line[static_cast<size_t>(p)] = input[i];
                if (++p >= len)
                    p = 0;
            }
            endPos = p;
        }
        dryPos = endPos;
    }

    // Looks at the newest pitch measurement and works out the correction to apply.
    // Called once per detection hop.
    void updateCorrection (int hop)
    {
        const double hopSeconds = static_cast<double>(hop) / std::max(1.0, sr);
        keyEstimator.advanceTime(hopSeconds);
        keyEstimateSampleCounter += hop;
        if (keyEstimateSampleCounter >= static_cast<int>(std::max(1.0, sr * 0.25)))
        {
            keyEstimateSampleCounter = 0;
            publishKeyEstimate();
        }

        // Smooth host automation of Humanize independently from pitch correction. The
        // correction smoother already controls retune speed; this short control smoother
        // prevents a rapid knob/automation jump from changing correction depth abruptly.
        const double humanizeCoeff = std::exp(-static_cast<double>(hop)
                                               / (std::max(1.0, sr) * 0.015));
        smoothedHumanize = smoothedHumanize * humanizeCoeff
                         + static_cast<double>(std::clamp(humanize, 0.0f, 1.0f))
                           * (1.0 - humanizeCoeff);
        smoothedFlexTune = smoothedFlexTune * humanizeCoeff
                         + static_cast<double>(std::clamp(flexTune, 0.0f, 1.0f))
                           * (1.0 - humanizeCoeff);

        const float observedHz = detector.detectPitch();

        const bool voiced = observedHz >= detector.getMinFrequency()
                         && observedHz <= PitchDetector::kMaxHz
                         && detector.getConfidence() >= 0.3f;

        if (!voiced)
        {
            lastDetectedHz = observedHz;
            resetPitchMotionTracker();
            lastTargetHz = observedHz;
            lastCorrectionCents = 0.0f;
            lastDetectedMidi = -1;
            lastTargetMidi = -1;
            lastTargetMidiFloat = -1.0f;

            // At voiced/unvoiced boundaries, a single low-confidence hop is common
            // (breath, consonant, or a weak cycle). Avoid hard-snapping grain spacing
            // to unity in one hop: release the current correction smoothly over a few
            // milliseconds. If the gap persists, let the ratio reach unity and discard
            // the note lock instead of carrying correction through extended silence.
            ++unvoicedHopCount;
            constexpr double releaseSeconds = 0.0045;
            const double releaseCoeff = std::exp(-static_cast<double>(hop)
                                                  / (std::max(1.0, sr) * releaseSeconds));
            smoothedCorrectionCents *= releaseCoeff;
            if (std::abs(smoothedCorrectionCents) < 1.0)
                smoothedCorrectionCents = 0.0;

            float ratio = std::pow(2.0f, static_cast<float>(smoothedCorrectionCents) / 1200.0f);
            if (std::abs(ratio - 1.0f) < 0.0008f)
                ratio = 1.0f;
            curRatio = ratio;
            curPeriod = (unvoicedHopCount <= 2 && lastVoicedPeriodSamples > 0.0)
                      ? static_cast<float>(lastVoicedPeriodSamples) : 0.0f;

            if (unvoicedHopCount >= 3)
            {
                lockedMidiNote = -1;
                holdCounter = 0;
            }
            // Preserve target glide across brief consonants and detector dropouts, but
            // reset it after a genuine pause so the next phrase starts on its first note.
            const int glideResetHops = std::max(3, static_cast<int>(0.12 * sr / std::max(1, hop)));
            if (unvoicedHopCount == glideResetHops)
                targetGlide.reset();
            return;
        }

        // A confident voiced frame re-arms the short dropout hold and stores its last
        // valid cycle period in host-sample units for any following brief unvoiced gap.
        unvoicedHopCount = 0;
        lastVoicedPeriodSamples = static_cast<double>(sr) / observedHz;

        // The YIN estimate is accurate on steady notes, but it describes a past
        // window (and then passes through a 3-frame median). Predict its pitch
        // contour to the current input time so the shifter receives a timely ratio
        // during vibrato and legato glides. Large note jumps reset the predictor,
        // preventing the forecast from shooting across octaves or scale targets.
        const float midiNote = 69.0f + 12.0f * std::log2(observedHz / referenceFreq);
        keyEstimator.observePitch(midiNote, detector.getConfidence(), hopSeconds);
        const float predictedMidiNote = predictPitchMotion(midiNote, observedHz,
                                                           detector.getConfidence(), hop);
        const float detectedHz = referenceFreq
            * std::pow(2.0f, (predictedMidiNote - 69.0f) / 12.0f);
        lastDetectedHz = detectedHz;
        lastDetectedMidi = static_cast<int>(std::round(predictedMidiNote));

        // Keep scale quantization on the measured pitch rather than the prediction.
        // That way anticipation improves the applied ratio without making the target
        // note change early as a singer approaches a note boundary.
        float targetMidi = quantizeToScale(midiNote);

        // Note stabilizer: stay on the locked note while the voice is within sustainCents of it
        if (noteStabilizer && lockedMidiNote >= 0)
        {
            const float lockedHz = referenceFreq * std::pow(2.0f, (lockedMidiNote - 69.0f) / 12.0f);
            const float centsDiff = std::abs(1200.0f * std::log2(observedHz / lockedHz));

            if (centsDiff < sustainCents)
            {
                targetMidi = lockedMidiNote;
                holdCounter++;
            }
            else
            {
                holdCounter = 0;
                lockedMidiNote = targetMidi;
            }
        }
        else
        {
            lockedMidiNote = targetMidi;
        }

        const float appliedTargetMidi = targetGlide.process(targetMidi, glideMilliseconds, hopSeconds);
        lastTargetMidiFloat = appliedTargetMidi;
        lastTargetMidi = static_cast<int>(std::round(appliedTargetMidi));
        const float targetHz = referenceFreq * std::pow(2.0f, (appliedTargetMidi - 69.0f) / 12.0f);
        lastTargetHz = targetHz;

        const float observedCorrectionCents = 1200.0f * std::log2(targetHz / observedHz);
        float correctionCents = 1200.0f * std::log2(targetHz / detectedHz);
        lastCorrectionCents = correctionCents;

        correctionCents *= amount;               // correction depth
        correctionCents *= (1.0f - static_cast<float>(smoothedHumanize)); // smoothed humanize
        // Flex-Tune describes the singer's measured excursion from the note, not
        // the predictor's forecast. Keep its bend-preservation curve tied to the
        // observed pitch so prediction cannot accidentally reduce Flex correction.
        correctionCents *= flexCorrectionWeight(std::abs(observedCorrectionCents),
                                                 static_cast<float>(smoothedFlexTune));

        // Retune speed: time constant of a one-pole smoother on the correction
        double timeConstant;
        if (retuneSpeed < 0.01f)
            timeConstant = 0.00008;     // nearly instant
        else if (retuneSpeed < 0.1f)
            timeConstant = 0.0003 + static_cast<double>(retuneSpeed) * 0.0005;
        else if (retuneSpeed < 0.5f)
            timeConstant = 0.0005 + static_cast<double>(retuneSpeed) * 0.05;
        else
            timeConstant = 0.02 + static_cast<double>(retuneSpeed) * 0.08;

        const double coeff = std::exp(-static_cast<double>(hop) / (sr * timeConstant));
        smoothedCorrectionCents = smoothedCorrectionCents * coeff
                                + static_cast<double>(correctionCents) * (1.0 - coeff);

        float ratio = std::pow(2.0f, static_cast<float>(smoothedCorrectionCents) / 1200.0f);
        if (std::abs(ratio - 1.0f) < 0.0008f)
            ratio = 1.0f;                        // too small to matter

        curRatio  = ratio;
        curPeriod = static_cast<float>(sr) / detectedHz;
    }

    void applyLatencyMode()
    {
        const float minHz = kRangeMinHz[rangeIndex];

        detector.setMinFrequency(minHz);
        for (auto& s : shifters)
            s.setMinFrequency(static_cast<double>(minHz));

        latencySamples = shifters[0].getLatencySamples();

        for (auto& line : dryLine)
            line.assign(static_cast<size_t>(latencySamples), 0.0f);
        dryPos = 0;

        smoothedCorrectionCents = 0.0;
        smoothedHumanize = static_cast<double>(std::clamp(humanize, 0.0f, 1.0f));
        smoothedFlexTune = static_cast<double>(std::clamp(flexTune, 0.0f, 1.0f));
        targetGlide.reset();
        lockedMidiNote = -1;
        holdCounter = 0;
        resetPitchMotionTracker();
        samplesSinceHop = 0;
        unvoicedHopCount = 0;
        lastVoicedPeriodSamples = 0.0;
        curRatio = 1.0f;
        curPeriod = 0.0f;

        lastDetectedHz = lastTargetHz = lastCorrectionCents = 0.0f;
        lastDetectedMidi = lastTargetMidi = -1;
        lastTargetMidiFloat = -1.0f;
    }

    void publishKeyEstimate() noexcept
    {
        const auto e = keyEstimator.estimate();
        estimatedKeyRoot.store(e.root, std::memory_order_relaxed);
        estimatedKeyMinor.store(e.minor ? 1 : 0, std::memory_order_relaxed);
        estimatedKeyConfidence.store(e.confidence, std::memory_order_relaxed);
        estimatedKeyEvidence.store(e.evidenceSeconds, std::memory_order_relaxed);
        estimatedKeyScore.store(e.profileScore, std::memory_order_relaxed);
        estimatedKeyMargin.store(e.scoreMargin, std::memory_order_relaxed);
        estimatedKeyDistinct.store(e.distinctPitchClasses, std::memory_order_relaxed);
        estimatedKeyValid.store(e.valid ? 1 : 0, std::memory_order_relaxed);
    }

    float quantizeToScale (float midiNote) const
    {
        // A manually edited piano overrides the named scale and remains 12-TET.
        // Otherwise use the named scale's exact intervals, including quarter tones.
        return useCustom ? ScaleQuantizer::quantizeMidiToMask(midiNote, customScale)
                         : ScaleQuantizer::quantizeMidi(midiNote, rootNote, scaleType);
    }

    double sr = 44100.0;
    PitchDetector detector;
    std::array<PitchShifter, kMaxChannels> shifters;
    std::vector<float> detectorMono;

    int   rootNote       = 0;
    int   scaleType      = 0;
    float retuneSpeed    = 0.0f;
    float amount         = 1.0f;     // full correction (the Amount knob was removed)
    float humanize       = 0.0f;
    float flexTune       = 0.0f;
    float glideMilliseconds = 0.0f;
    float sustainCents   = 60.0f;     // Note Stabilizer: small hysteresis so vibrato near a
                                      // note boundary does not make the target flip (the Sustain knob was removed)
    bool  noteStabilizer = false;
    bool  formantPreserve = true;
    float formantShiftSemitones = 0.0f;
    float referenceFreq  = 440.0f;
    bool  enabled        = true;
    bool  lowLatencyMode = false;
    int   rangeIndex     = kDefaultRange;
    bool  prepared       = false;
    bool  useCustom      = false;
    std::array<bool, 12> customScale = kChromatic;

    double smoothedCorrectionCents = 0.0;
    bool   pitchMotionHasPrevious = false;
    float  previousObservedMidi = 0.0f;
    double smoothedHumanize = 0.0;
    double smoothedFlexTune = 0.0;
    TargetGlide targetGlide;
    KeyEstimator keyEstimator;
    int keyEstimateSampleCounter = 0;
    std::atomic<int> estimatedKeyRoot { 0 };
    std::atomic<int> estimatedKeyMinor { 0 };
    std::atomic<float> estimatedKeyConfidence { 0.0f };
    std::atomic<float> estimatedKeyEvidence { 0.0f };
    std::atomic<float> estimatedKeyScore { 0.0f };
    std::atomic<float> estimatedKeyMargin { 0.0f };
    std::atomic<int> estimatedKeyDistinct { 0 };
    std::atomic<int> estimatedKeyValid { 0 };
    float  lockedMidiNote = -1.0f;
    int    holdCounter    = 0;
    int    latencySamples = 0;
    int    samplesSinceHop = 0;     // samples fed to the detector since its last measurement
    int    unvoicedHopCount = 0;    // short dropout hysteresis before releasing note lock
    double lastVoicedPeriodSamples = 0.0;
    float  curRatio  = 1.0f;        // correction being applied right now
    float  curPeriod = 0.0f;

    std::array<std::vector<float>, kMaxChannels> dryLine;
    int dryPos = 0;

    float lastDetectedHz      = 0.0f;
    float lastTargetHz        = 0.0f;
    float lastCorrectionCents = 0.0f;
    int   lastDetectedMidi    = -1;
    int   lastTargetMidi      = -1;
    float lastTargetMidiFloat = -1.0f;
};

} // namespace humtune
