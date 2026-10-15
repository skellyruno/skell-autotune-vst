#pragma once

#include "HumHouseLookAndFeel.h"
#include "PluginProcessor.h"
#include <JuceHeader.h>

// SkellyTune interface.
//
// All positions in PluginEditor.cpp are written for a "design size" of 780 x 620.
// The window is shown at 75% of that by default and can be resized by dragging the
// bottom-right corner; everything scales together.
//
// Layout (top to bottom):
//   - Top bar: logo, title, Enabled switch
//   - Radar visualizer with the skull (centre), skeleton arms on either side,
//     In/Out gain (left), Retune Speed + Humanize (right)
//   - Key, scale buttons, Note Stabilizer / Formant switches, Voice Range
//   - Pitch heatmap + piano keyboard (the piano keys are the scale)
//   - Monitor panel: level meters, pitch readouts, pitch reference

class HumHouseVocalTuneEditor : public juce::AudioProcessorEditor,
                                public juce::AudioProcessorParameter::Listener,
                                private juce::Timer
{
public:
    explicit HumHouseVocalTuneEditor (HumHouseVocalTuneProcessor&);
    ~HumHouseVocalTuneEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    // Implement listener interface
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;

private:
    void timerCallback() override;

    static constexpr int kDesignW = 780;
    static constexpr int kDesignH = 620;
    float scaleFactor() const { return static_cast<float>(getWidth()) / static_cast<float>(kDesignW); }

    HumHouseVocalTuneProcessor& proc;
    humtune::HumHouseLookAndFeel lnf;
    juce::TooltipWindow tooltipWindow { this, 500 };

    // Artwork shown as small child components so they are drawn crisply at any size
    struct ArtView : public juce::Component
    {
        ArtView() { setInterceptsMouseClicks(false, false); }
        void paint (juce::Graphics& g) override
        {
            if (image.isValid())
            {
                g.setOpacity(opacity);
                g.drawImageAt(image, 0, 0);
            }
        }
        juce::Image image;
        float opacity = 1.0f;
    };

    struct SkullView : public juce::Component
    {
        SkullView() { setInterceptsMouseClicks(false, false); }
        void paint (juce::Graphics& g) override
        {
            if (solid.isValid())
            {
                g.setOpacity(0.9f);
                g.drawImageAt(solid, 0, 0);
            }
            if (holo.isValid() && glow > 0.02f)
            {
                g.setOpacity(0.75f * glow);
                g.drawImageAt(holo, 0, 0);
            }
        }
        juce::Image solid, holo;
        float glow = 0.0f;
    };

    ArtView armLeftView, armRightView;
    SkullView skullView;
    juce::Image skullSolidImg, skullHoloImg, armImg;

    // Knobs
    juce::Slider speedKnob, humanizeKnob;
    juce::Slider inputGainKnob, outputGainKnob;
    juce::Slider referenceFreqKnob;

    // Labels for knobs
    juce::Label speedLabel, humanizeLabel;
    juce::Label inputGainLabel, outputGainLabel, referenceFreqLabel;

    // Buttons
    juce::TextButton majorBtn {"Major"};
    juce::TextButton minorBtn {"Minor"};
    juce::TextButton chromBtn {"Chromatic"};

    juce::ToggleButton stabilizerBtn {"Note Stabilizer"};
    juce::ToggleButton formantBtn    {"Formant"};
    juce::ToggleButton lowLatencyBtn {"Low Latency"};
    juce::ToggleButton enableBtn     {"Enabled"};

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
    using ComboAttach  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<SliderAttach> speedAtt;
    std::unique_ptr<SliderAttach> humanizeAtt;
    std::unique_ptr<SliderAttach> inputGainAtt, outputGainAtt, referenceFreqAtt;
    std::unique_ptr<ButtonAttach> stabilizerAtt, formantAtt, lowLatencyAtt, enableAtt;
    std::unique_ptr<ComboAttach>  rootNoteAtt, rangeAtt;
    std::array<std::unique_ptr<ButtonAttach>, 12> noteAtts;

    // Pitch display state
    float displayDetectedHz      = 0.0f;
    float displayTargetHz        = 0.0f;
    float displayCorrectionCents = 0.0f;
    float displayConfidence      = 0.0f;
    int   displayDetectedMidi    = -1;
    int   displayTargetMidi      = -1;
    float displayInputLevel      = 0.0f;
    float displayOutputLevel     = 0.0f;
    float meterIn                = 0.0f;   // smoothed 0..1 meter positions
    float meterOut               = 0.0f;

    // Pitch heatmap history (scrolling)
    static constexpr int kHeatmapWidth = 200;
    std::array<float, kHeatmapWidth> heatmapDetected {};
    std::array<float, kHeatmapWidth> heatmapTarget {};
    int heatmapWriteIdx = 0;

    void setupKnob (juce::Slider&, juce::Label&, const juce::String& text, const juce::String& tooltip);
    void rebuildScaledArt();
    void applyScaleFromControls();
    int  currentScaleType() const;
    int  currentRoot() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumHouseVocalTuneEditor)
};
