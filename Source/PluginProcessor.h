#pragma once

#include "AutoTuneEngine.h"
#include <JuceHeader.h>
#include <array>
#include <atomic>

class HumHouseVocalTuneProcessor : public juce::AudioProcessor,
                                     private juce::AsyncUpdater
{
public:
    HumHouseVocalTuneProcessor();
    ~HumHouseVocalTuneProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms() override;
    int  getCurrentProgram() override { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // True if pitch class `note` (0 = C ... 11 = B) belongs to the scale in the given key.
    // scaleType: 0 = Major, 1 = Minor, 2 = Chromatic.
    static bool isNoteInScale (int scaleType, int root, int note)
    {
        static constexpr bool major[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
        static constexpr bool minor[12] = { true, false, true, true, false, true, false, true, true, false, true, false };
        const int rel = (((note - root) % 12) + 12) % 12;
        return scaleType == 2 ? true : (scaleType == 1 ? minor[rel] : major[rel]);
    }

    // The piano keys ARE the scale: this lights the keys that belong to the chosen scale + key.
    void applyScaleToKeys (int scaleType, int root);

    // Readbacks for editor
    float getDetectedHz()       const { return engine.getDetectedHz(); }
    float getTargetHz()         const { return engine.getTargetHz(); }
    float getCorrectionCents()  const { return engine.getCorrectionCents(); }
    float getConfidence()       const { return engine.getConfidence(); }
    int   getDetectedMidiNote() const { return engine.getDetectedMidiNote(); }
    int   getTargetMidiNote()   const { return engine.getTargetMidiNote(); }

    // Input/output levels for meters
    float getInputLevel()  const { return inputLevel.load(); }
    float getOutputLevel() const { return outputLevel.load(); }

    // Note names for display
    static juce::String midiNoteToName (int midiNote)
    {
        if (midiNote < 0) return "";
        static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        int octave = (midiNote / 12) - 1;
        return juce::String(names[midiNote % 12]) + juce::String(octave);
    }

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void cacheParameterRefs();

    // Resolve APVTS IDs off the audio thread. In particular, the note-key IDs are
    // dynamic strings; constructing them in processBlock would create avoidable work.
    struct ParameterRefs
    {
        std::atomic<float>* retune = nullptr;
        std::atomic<float>* humanize = nullptr;
        std::atomic<float>* rootNote = nullptr;
        std::atomic<float>* scaleType = nullptr;
        std::atomic<float>* referenceFreq = nullptr;
        std::atomic<float>* noteStabilizer = nullptr;
        std::atomic<float>* formantPreserve = nullptr;
        std::atomic<float>* range = nullptr;
        std::atomic<float>* enabled = nullptr;
        std::atomic<float>* lowLatencyMode = nullptr;
        std::atomic<float>* inputGain = nullptr;
        std::atomic<float>* outputGain = nullptr;
        std::array<std::atomic<float>*, 12> notes{};
    } parameterRefs;

    // Tells the host about a latency change (must happen on the message thread)
    void handleAsyncUpdate() override { setLatencySamples(pendingLatency.load()); }
    std::atomic<int> pendingLatency { 0 };
    int reportedLatency = 0;

    juce::AudioProcessorValueTreeState apvts;
    humtune::AutoTuneEngine engine;

    std::atomic<float> inputLevel  { 0.0f };
    std::atomic<float> outputLevel { 0.0f };

    // Default program index points at the verified quality-first program appended last.
    static constexpr int kStudioMaxProgramIndex = 12;
    int currentPreset = kStudioMaxProgramIndex;

    struct Preset
    {
        juce::String name;
        float speed;
        float humanize;
        int   scaleType;
        bool  stabilizer;
        bool  formantPreserve;
        int   range;        // voice range 0..5, or -1 to leave the user's choice alone
        int   lowLatencyMode = -1; // -1 preserves existing behavior; 0 = Standard, 1 = experimental
    };

    static const std::vector<Preset>& getPresets();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumHouseVocalTuneProcessor)
};
