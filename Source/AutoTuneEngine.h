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

    // Voice range = the lowest note the tuner follows. The delay is approximately half a
    // period plus 8 samples. At 44.1/48 kHz this is about:
    //    70 Hz -> 7.3 ms     80 Hz -> 6.4 ms    100 Hz -> 5.2 ms
    //   130 Hz -> 4.0 ms    180 Hz -> 3.0 ms
    // The low-latency trade-off is explicit: notes below the selected floor are not tuned.
    static constexpr int   kNumRanges    = 5;
    static constexpr float kRangeMinHz[kNumRanges] = { 70.0f, 80.0f, 100.0f, 130.0f, 180.0f };
    static constexpr int   kDefaultRange = 4;

    void prepare (double sampleRate, int blockSize)
    {
        sr = sampleRate;
        detector.prepare(sampleRate, blockSize);
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
    void setEnabled         (bool on)     { enabled = on; }

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

        // The correction is refreshed every `hop` samples (about 2.5 ms) instead of once
        // per DAW buffer, and each piece of audio is shifted straight after the pitch
        // of that piece was measured. That keeps the correction in step with the voice.
        const int   hop  = detector.getHopSize();
        const float* mono = buffer.getReadPointer(0);

        int pos = 0;
        while (pos < numSamples)
        {
            const int seg = std::min(numSamples - pos, hop - samplesSinceHop);

            detector.feedSamples(mono + pos, seg);
            samplesSinceHop += seg;
            if (samplesSinceHop >= hop)
            {
                samplesSinceHop = 0;
                updateCorrection(hop);
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

        correctionCents *= amount;               // correction depth
        correctionCents *= (1.0f - humanize);    // humanize

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
        lockedMidiNote = -1;
        holdCounter = 0;
        samplesSinceHop = 0;
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

    int   rootNote       = 0;
    int   scaleType      = 0;
    float retuneSpeed    = 0.0f;
    float amount         = 1.0f;     // full correction (the Amount knob was removed)
    float humanize       = 0.0f;
    float sustainCents   = 60.0f;     // Note Stabilizer: small hysteresis so vibrato near a
                                      // note boundary does not make the target flip (the Sustain knob was removed)
    bool  noteStabilizer = true;
    bool  formantPreserve = true;
    float referenceFreq  = 440.0f;
    bool  enabled        = true;
    int   rangeIndex     = kDefaultRange;
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
