#pragma once

#include "PitchDetector.h"
#include "PitchShifter.h"
#include <JuceHeader.h>
#include <array>
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
        applyLatencyMode();
    }

    int   getLatencySamples() const { return latencySamples; }
    float getMinFrequency()   const { return detector.getMinFrequency(); }

    void setRootNote        (int note)    { rootNote = note % 12; }
    void setScaleType       (int type)    { scaleType = type; }
    void setRetuneSpeed     (float spd)   { retuneSpeed = spd; }
    void setAmount          (float amt)   { amount = amt; }
    void setHumanize        (float h)     { humanize = h; }
    void setSustain         (float s)     { sustainCents = s; }
    void setNoteStabilizer  (bool on)     { noteStabilizer = on; }
    void setFormantPreserve (bool on)     { formantPreserve = on; }
    void setReferenceFreq   (float hz)    { referenceFreq = hz; }
    void setEnabled (bool on)
    {
        if (enabled == on)
            return;

        enabled = on;
        smoothedCorrectionCents = 0.0;
        smoothedHumanize = static_cast<double>(std::clamp(humanize, 0.0f, 1.0f));
        lockedMidiNote = -1;
        holdCounter = 0;
        samplesSinceHop = 0;
        unvoicedHopCount = 0;
        lastVoicedPeriodSamples = 0.0;
        curRatio = 1.0f;
        curPeriod = 0.0f;
        lastDetectedHz = lastTargetHz = lastCorrectionCents = 0.0f;
        lastDetectedMidi = lastTargetMidi = -1;

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
    int   getTargetMidiNote()   const { return lastTargetMidi; }

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
        // Smooth host automation of Humanize independently from pitch correction. The
        // correction smoother already controls retune speed; this short control smoother
        // prevents a rapid knob/automation jump from changing correction depth abruptly.
        const double humanizeCoeff = std::exp(-static_cast<double>(hop)
                                               / (std::max(1.0, sr) * 0.015));
        smoothedHumanize = smoothedHumanize * humanizeCoeff
                         + static_cast<double>(std::clamp(humanize, 0.0f, 1.0f))
                           * (1.0 - humanizeCoeff);

        const float detectedHz = detector.detectPitch();
        lastDetectedHz = detectedHz;

        const bool voiced = detectedHz >= detector.getMinFrequency()
                         && detectedHz <= PitchDetector::kMaxHz
                         && detector.getConfidence() >= 0.3f;

        if (!voiced)
        {
            lastTargetHz = detectedHz;
            lastCorrectionCents = 0.0f;
            lastDetectedMidi = -1;
            lastTargetMidi = -1;

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
            return;
        }

        // A confident voiced frame re-arms the short dropout hold and stores its last
        // valid cycle period in host-sample units for any following brief unvoiced gap.
        unvoicedHopCount = 0;
        lastVoicedPeriodSamples = static_cast<double>(sr) / detectedHz;

        const float midiNote = 69.0f + 12.0f * std::log2(detectedHz / referenceFreq);
        lastDetectedMidi = static_cast<int>(std::round(midiNote));

        int targetMidi = quantizeToScale(midiNote);

        // Note stabilizer: stay on the locked note while the voice is within sustainCents of it
        if (noteStabilizer && lockedMidiNote >= 0)
        {
            const float lockedHz = referenceFreq * std::pow(2.0f, (static_cast<float>(lockedMidiNote) - 69.0f) / 12.0f);
            const float centsDiff = std::abs(1200.0f * std::log2(detectedHz / lockedHz));

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

        lastTargetMidi = targetMidi;
        const float targetHz = referenceFreq * std::pow(2.0f, (static_cast<float>(targetMidi) - 69.0f) / 12.0f);
        lastTargetHz = targetHz;

        float correctionCents = 1200.0f * std::log2(targetHz / detectedHz);
        lastCorrectionCents = correctionCents;

        correctionCents *= amount;               // correction depth
        correctionCents *= (1.0f - static_cast<float>(smoothedHumanize)); // smoothed humanize

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
        lockedMidiNote = -1;
        holdCounter = 0;
        samplesSinceHop = 0;
        unvoicedHopCount = 0;
        lastVoicedPeriodSamples = 0.0;
        curRatio = 1.0f;
        curPeriod = 0.0f;

        lastDetectedHz = lastTargetHz = lastCorrectionCents = 0.0f;
        lastDetectedMidi = lastTargetMidi = -1;
    }

    int quantizeToScale (float midiNote) const
    {
        const auto& scale = useCustom ? customScale
                          : (scaleType == 1) ? kMinor
                          : (scaleType == 2) ? kChromatic
                          : kMajor;

        const int nearest = static_cast<int>(std::round(midiNote));

        // Major / Minor are measured from the chosen Key. The piano keys (custom scale)
        // are labelled C..B, so they are absolute and must not move with the Key.
        const int noteInOctave = useCustom ? ((nearest % 12) + 120) % 12
                                           : ((nearest % 12) - rootNote + 120) % 12;

        if (scale[static_cast<size_t>(noteInOctave)])
            return nearest;

        for (int offset = 1; offset <= 6; ++offset)
        {
            const int up   = (noteInOctave + offset) % 12;
            const int down = (noteInOctave - offset + 12) % 12;

            const bool upOk   = scale[static_cast<size_t>(up)];
            const bool downOk = scale[static_cast<size_t>(down)];

            if (upOk && downOk)
            {
                const float fracPart = midiNote - static_cast<float>(nearest);
                return (fracPart >= 0.0f) ? nearest + offset : nearest - offset;
            }
            if (upOk)   return nearest + offset;
            if (downOk) return nearest - offset;
        }

        return nearest;
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
    float sustainCents   = 60.0f;     // Note Stabilizer: small hysteresis so vibrato near a
                                      // note boundary does not make the target flip (the Sustain knob was removed)
    bool  noteStabilizer = false;
    bool  formantPreserve = true;
    float referenceFreq  = 440.0f;
    bool  enabled        = true;
    bool  lowLatencyMode = false;
    int   rangeIndex     = kDefaultRange;
    bool  prepared       = false;
    bool  useCustom      = false;
    std::array<bool, 12> customScale = kChromatic;

    double smoothedCorrectionCents = 0.0;
    double smoothedHumanize = 0.0;
    int    lockedMidiNote = -1;
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
};

} // namespace humtune
