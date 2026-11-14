#pragma once

#include "HumHouseLookAndFeel.h"
#include "PluginProcessor.h"
#include "SkeletonOrb.h"
#include <JuceHeader.h>

// Material-themed interface with physical dials, a voiced-activity skeleton orb,
// original/target pitch history, and a full-width scale keyboard.
// Note Stabilizer defaults off; its legacy parameter remains available for saved-session
// compatibility. Input/output gain controls are omitted from this focused interface.

class HumHouseVocalTuneEditor : public juce::AudioProcessorEditor, private juce::Timer {
  public:
    explicit HumHouseVocalTuneEditor(HumHouseVocalTuneProcessor &);
    ~HumHouseVocalTuneEditor() override;

    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    void resized() override;

  private:
    void timerCallback() override;

    static constexpr int kDesignW = 700;
    static constexpr int kDesignH = 680;
    float scaleFactor() const { return static_cast<float>(getWidth()) / static_cast<float>(kDesignW); }

    HumHouseVocalTuneProcessor &proc;
    humtune::HumHouseLookAndFeel lnf;
    juce::TooltipWindow tooltipWindow{this, 500};

    SkeletonOrb orb;
    juce::TextButton appearanceButton{""};
    juce::TextButton settingsButton{""};
    juce::Random lightingRandom;
    int lightingHold = 0, flareHold = 180;
    float lightingTarget = .5f, lightingLevel = .5f, lightingFlare = 0;
    double lightingPhase = 0;
    int appearanceTheme = -1;
    void applyAppearance();

    // Knobs
    juce::Slider speedKnob, humanizeKnob, referenceFreqKnob;

    // Labels for knobs
    juce::Label referenceFreqLabel;

    // Scale selector (western scales plus optional 24-TET and Maqam modes)
    juce::ComboBox scaleBox;

    juce::ToggleButton enableBtn{"Enabled"};

    // Voice range: the lowest note the tuner follows (lower notes need more delay)
    juce::ComboBox rangeBox;

    // Key (root note) selector
    juce::ComboBox rootNoteBox;
    juce::Label rootNoteLabel;

    // Piano keyboard note buttons (C..B). Lit = the tuner may snap to this note.
    std::array<juce::TextButton, 12> noteButtons;

    // APVTS attachments
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<SliderAttach> speedAtt;
    std::unique_ptr<SliderAttach> humanizeAtt, referenceFreqAtt;
    std::unique_ptr<ButtonAttach> enableAtt;
    std::unique_ptr<ComboAttach> rootNoteAtt;
    int lastDisplayedVoice = -1;
    void refreshVoiceType(int voice);
    int lastAppliedScale = -1;
    int lastAppliedRoot = -1;
    std::array<std::unique_ptr<ButtonAttach>, 12> noteAtts;

    // Pitch display state
    float displayDetectedHz = 0.0f;
    float displayTargetHz = 0.0f;
    float displayConfidence = 0.0f;
    int displayDetectedMidi = -1;
    int displayTargetMidi = -1;
    float displayInputLevel = 0.0f;
    // Pitch heatmap history (scrolling)
    static constexpr int kHeatmapWidth = 200;
    std::array<float, kHeatmapWidth> heatmapDetected{};
    std::array<float, kHeatmapWidth> heatmapTarget{};
    int heatmapWriteIdx = 0;

    void setupKnob(juce::Slider &, const juce::String &text, const juce::String &tooltip);
    void applyScaleFromControls();
    int currentScaleType() const;
    int currentRoot() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HumHouseVocalTuneEditor)
};
