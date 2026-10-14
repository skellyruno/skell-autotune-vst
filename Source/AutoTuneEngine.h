#pragma once

#include "PitchDetector.h"
#include "PitchShifter.h"
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace humtune
{

class AutoTuneEngine
{
public:
    static constexpr std::array<bool, 12> kMajor           = {true,false,true,false,true,true,false,true,false,true,false,true};
    static constexpr std::array<bool, 12> kMinor           = {true,false,true,true,false,true,false,true,true,false,true,false};
    static constexpr std::array<bool, 12> kChromatic       = {true,true,true,true,true,true,true,true,true,true,true,true};
    static constexpr std::array<bool, 12> kHarmonicMinor   = {true,false,true,true,false,true,false,true,true,false,false,true};
    static constexpr std::array<bool, 12> kMajorPentatonic = {true,false,true,false,true,false,false,true,false,true,false,false};
    static constexpr std::array<bool, 12> kMinorPentatonic = {true,false,false,true,false,true,false,true,false,false,true,false};
    static constexpr std::array<bool, 12> kBlues           = {true,false,false,true,false,true,true,true,false,false,true,false};

    static constexpr int kMaxChannels = 2;

    // Voice range = the lowest note the tuner follows. The low-latency TD-PSOLA shifter
    // uses real samples and a reduced future-sample requirement. At 48 kHz, reported latency
    // is approximately 16.6 / 14.5 / 11.7 / 9.0 / 6.6 ms for 70 / 80 / 100 / 130 / 180 Hz.
    static constexpr int   kNumRanges    = 5;
    static constexpr float kRangeMinHz[kNumRanges] = { 70.0f, 80.0f, 100.0f, 130.0f, 180.0f };
    static constexpr int   kDefaultRange = 4;

    void prepare (double sampleRate, int blockSize)
    {
        sr = sampleRate;
        detector.prepare(sampleRate, blockSize);
        monoScratch.assign(static_cast<size_t>(std::max(1, detector.getHopSize())), 0.0f);
        transientEnergySum = 0.0;
        transientSampleCount = 0;
        smoothedInputRms = 0.0f;
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

    void setRootNote        (int note)    { rootNote = ((note % 12) + 12) % 12; }
    void setScaleType       (int type)    { scaleType = juce::jlimit(0, 6, type); }
    void setRetuneSpeed     (float spd)   { retuneSpeed = spd; }
    void setAmount          (float amt)   { amount = amt; }
    void setHumanize        (float h)     { humanize = h; }
    void setSustain         (float s)     { sustainCents = s; }
    void setNoteStabilizer  (bool on)     { noteStabilizer = on; }
    void setFormantPreserve (bool on)     { formantPreserve = on; }
    void setReferenceFreq   (float hz)    { referenceFreq = hz; }
    void setEnabled         (bool on)     { enabled = on; }
    void setLowLatencyMode  (bool on)
    {
        if (lowLatencyMode == on)
            return;
        lowLatencyMode = on;
        if (prepared)
        {
            // Profile-only change: preserve detector and shifter history. Input Type
            // still owns the pitch floor; the switch changes the shifter's delay profile.
            for (auto& shifter : shifters)
                shifter.configure(static_cast<double>(voiceFloorHz), lowLatencyMode);
            latencySamples = shifters[0].getLatencySamples();
        }
    }
    bool getLowLatencyMode() const { return lowLatencyMode; }

    // Chooses the voice range (index into kRangeMinHz). Safe to call from the audio thread
    // (no allocation). The latency changes, so the host must be told: see getLatencySamples().
    void setVoiceRange (int index)
    {
        index = std::max(0, std::min(kNumRanges - 1, index));
        rangeIndex = index;
        setVoiceFloor(kRangeMinHz[index]);
    }

    // A finer floor setting lets the Low Latency control trade low-note coverage for
    // algorithmic delay beyond the five broad voice presets. The shifter/detector buffers
    // are sized for 70 Hz in prepare(), so this does not allocate when the floor changes.
    void setVoiceFloor (float hz)
    {
        hz = juce::jlimit(kRangeMinHz[0], 400.0f, hz);
        if (std::abs(hz - voiceFloorHz) < 0.5f)
            return;
        voiceFloorHz = hz;
        if (prepared)
            applyLatencyMode();
    }

    void setCustomScale     (const std::array<bool, 12>& s) { customScale = s; useCustom = true; }
    void clearCustomScale   ()            { useCustom = false; }

    float getDetectedHz()       const { return lastDetectedHz; }
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

            for (int ch = 0; ch < nch; ++ch)
                shifters[static_cast<size_t>(ch)].process(buffer.getWritePointer(ch), numSamples, 0.0f, 1.0f);
            return;
        }

        // The correction is refreshed every `hop` samples (about 5 ms) instead of once
        // per DAW buffer, and each piece of audio is shifted straight after the pitch
        // of that piece was measured. That keeps the correction in step with the voice.
        const int hop = detector.getHopSize();

        int pos = 0;
        while (pos < numSamples)
        {
            const int seg = std::min(numSamples - pos, hop - samplesSinceHop);

            // Sum(L+R)/2 style monophonic detection feed. Keep the original stereo
            // channels intact for independent, phase-coherent pitch shifting.
            double segmentEnergy = 0.0;
            double channelEnergy[kMaxChannels] = { 0.0, 0.0 };
            for (int i = 0; i < seg; ++i)
            {
                float mono = 0.0f;
                for (int ch = 0; ch < nch; ++ch)
                {
                    const float sample = buffer.getReadPointer(ch)[pos + i];
                    mono += sample;
                    channelEnergy[ch] += static_cast<double>(sample) * sample;
                }
                mono /= static_cast<float>(nch);
                monoScratch[static_cast<size_t>(i)] = mono;
                segmentEnergy += static_cast<double>(mono) * mono;
            }

            // Use the requested Sum(L+R)/2 detector feed for normal stereo material,
            // but guard against phase-opposed stereo inputs. A near-cancelled downmix
            // otherwise looks like silence and can leave the shifter stuck on the last note.
            if (nch == 2 && seg > 0)
            {
                const double monoRms = std::sqrt(segmentEnergy / static_cast<double>(seg));
                const int dominantChannel = channelEnergy[1] > channelEnergy[0] ? 1 : 0;
                const double dominantRms = std::sqrt(channelEnergy[dominantChannel] / static_cast<double>(seg));
                if (dominantRms > 0.001 && monoRms < 0.30 * dominantRms)
                {
                    segmentEnergy = 0.0;
                    const float* fallback = buffer.getReadPointer(dominantChannel) + pos;
                    for (int i = 0; i < seg; ++i)
                    {
                        const float sample = fallback[i];
                        monoScratch[static_cast<size_t>(i)] = sample;
                        segmentEnergy += static_cast<double>(sample) * sample;
                    }
                }
            }

            detector.feedSamples(monoScratch.data(), seg);
            transientEnergySum += segmentEnergy;
            transientSampleCount += seg;

            samplesSinceHop += seg;
            if (samplesSinceHop >= hop)
            {
                samplesSinceHop = 0;
                const float hopRms = transientSampleCount > 0
                    ? static_cast<float>(std::sqrt(transientEnergySum / transientSampleCount)) : 0.0f;
                const bool transient = hopRms > 0.035f
                    && hopRms > 2.4f * std::max(smoothedInputRms, 0.006f);

                // Fast attack / slow release for the reference envelope. When a sharp
                // plosive or transient arrives, still update the pitch display/history,
                // but ease the shifter back toward delayed dry for this hop rather than
                // imprinting the previous vowel's pitch ratio onto the transient.
                if (hopRms > smoothedInputRms)
                    smoothedInputRms = 0.65f * smoothedInputRms + 0.35f * hopRms;
                else
                    smoothedInputRms = 0.92f * smoothedInputRms + 0.08f * hopRms;
                transientEnergySum = 0.0;
                transientSampleCount = 0;

                if (transient)
                {
                    detector.detectPitch();
                    curRatio = 1.0f;
                    curPeriod = 0.0f;
                }
                else
                {
                    updateCorrection(hop);
                }
            }

            for (int ch = 0; ch < nch; ++ch)
            {
                auto& s = shifters[static_cast<size_t>(ch)];
                s.setFormantPreserve(formantPreserve);
                s.process(buffer.getWritePointer(ch) + pos, seg, curPeriod, curRatio);
            }

            pos += seg;
        }
    }

private:
    // Looks at the newest pitch measurement and works out the correction to apply.
    // Called once per detection hop.
    void updateCorrection (int hop)
    {
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

            smoothedCorrectionCents *= 0.95;
            lockedMidiNote = -1;
            holdCounter = 0;

            curRatio  = 1.0f;
            curPeriod = 0.0f;
            return;
        }

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

        correctionCents *= juce::jlimit(0.0f, 1.0f, amount); // correction depth

        // Retune Speed maps to an intuitive 0-60 ms one-pole response. Humanize adds
        // response time instead of weakening the final correction (which would leave notes
        // permanently flat/sharp and behave like a mix knob).
        const double speedNorm = juce::jlimit(0.0, 1.0, static_cast<double>(retuneSpeed));
        const double speedMs = speedNorm < 0.001 ? 0.02 : 60.0 * std::pow(speedNorm, 1.2);
        const double timeConstant = std::max(0.00002, (speedMs + 45.0 * juce::jlimit(0.0f, 1.0f, humanize)) / 1000.0);

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
        const float minHz = voiceFloorHz;

        detector.setMinFrequency(minHz);
        for (auto& s : shifters)
            s.configure(static_cast<double>(minHz), lowLatencyMode);

        latencySamples = shifters[0].getLatencySamples();

        for (auto& line : dryLine)
            line.assign(static_cast<size_t>(latencySamples), 0.0f);
        dryPos = 0;

        smoothedCorrectionCents = 0.0;
        lockedMidiNote = -1;
        holdCounter = 0;
        samplesSinceHop = 0;
        transientEnergySum = 0.0;
        transientSampleCount = 0;
        smoothedInputRms = 0.0f;
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
                          : (scaleType == 3) ? kHarmonicMinor
                          : (scaleType == 4) ? kMajorPentatonic
                          : (scaleType == 5) ? kMinorPentatonic
                          : (scaleType == 6) ? kBlues
                          : kMajor;

        // Search the nearest allowed semitone by actual distance, rather than choosing
        // a direction from the fractional part of round(midiNote). The old logic could
        // snap an exactly centred pitch to the wrong side because float/YIN error made
        // a nominal tie appear microscopically flat or sharp.
        const int centre = static_cast<int>(std::round(midiNote));
        int bestNote = centre;
        float bestDistance = std::numeric_limits<float>::max();
        constexpr float tieTolerance = 0.025f; // 2.5 cents; include short-window YIN startup error at exact scale midpoints

        for (int offset = -6; offset <= 6; ++offset)
        {
            const int candidate = centre + offset;
            const int pitchClass = useCustom ? ((candidate % 12) + 12) % 12
                                             : ((candidate % 12) - rootNote + 120) % 12;
            if (! scale[static_cast<size_t>(pitchClass)])
                continue;

            const float distance = std::abs(midiNote - static_cast<float>(candidate));
            if (distance < bestDistance - tieTolerance
                || (std::abs(distance - bestDistance) <= tieTolerance && candidate > bestNote))
            {
                bestDistance = distance;
                bestNote = candidate;
            }
        }
        return bestNote;
    }

    double sr = 44100.0;
    PitchDetector detector;
    std::array<PitchShifter, kMaxChannels> shifters;
    std::vector<float> monoScratch; // fixed-size pitch-detection downmix; sized in prepare()
    double transientEnergySum = 0.0;
    int transientSampleCount = 0;
    float smoothedInputRms = 0.0f;

    int   rootNote       = 0;
    int   scaleType      = 0;
    float retuneSpeed    = 0.0f;
    float amount         = 1.0f;     // full correction (available to the user as Amount)
    float humanize       = 0.0f;
    float sustainCents   = 60.0f;     // Note Stabilizer: small hysteresis so vibrato near a
                                      // note boundary does not make the target flip (the Sustain knob was removed)
    bool  noteStabilizer = true;
    bool  formantPreserve = true;
    bool  lowLatencyMode = true;
    float referenceFreq  = 440.0f;
    bool  enabled        = true;
    int   rangeIndex     = kDefaultRange;
    float voiceFloorHz   = kRangeMinHz[kDefaultRange];
    bool  prepared       = false;
    bool  useCustom      = false;
    std::array<bool, 12> customScale = kChromatic;

    double smoothedCorrectionCents = 0.0;
    int    lockedMidiNote = -1;
    int    holdCounter    = 0;
    int    latencySamples = 0;
    int    samplesSinceHop = 0;     // samples fed to the detector since its last measurement
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
