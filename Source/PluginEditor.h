#pragma once

#include "HumHouseLookAndFeel.h"
#include "PluginProcessor.h"
#include <JuceHeader.h>

// SkellyTune interface: left Retune Speed dial, central skull/radar and live pitch
// monitor, right Humanize dial, then key/scale controls and a full-width pitch keyboard.
// Note Stabilizer defaults off; its legacy parameter remains available for saved-session
// compatibility. Input/output gain controls are omitted from this focused interface.

class HumHouseVocalTuneEditor : public juce::AudioProcessorEditor,
                                private juce::Timer
{
public:
    explicit HumHouseVocalTuneEditor (HumHouseVocalTuneProcessor&);
    ~HumHouseVocalTuneEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintBackground(juce::Graphics&);
    juce::Image backgroundCache;
    bool popupWasVisible = false;

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
    juce::Slider speedKnob, humanizeKnob, flexTuneKnob, glideKnob;
    juce::Slider referenceFreqKnob, formantShiftKnob;

    // Labels for knobs
    juce::Label speedLabel, humanizeLabel, flexTuneLabel, glideLabel, referenceFreqLabel, formantShiftLabel;

    // Factory programs (keep a visible in-plugin route to the quality-first profile).
    juce::ComboBox presetBox;
    juce::TextButton autoKeyButton { "AUTO KEY" };
    int autoKeyFeedbackTicks = 0;

    // Scale selector (western scales plus optional 24-TET and Maqam modes)
    juce::ComboBox scaleBox;

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
    std::unique_ptr<SliderAttach> humanizeAtt, flexTuneAtt, glideAtt, referenceFreqAtt, formantShiftAtt;
    std::unique_ptr<ButtonAttach> formantAtt, lowLatencyAtt, enableAtt;
    std::unique_ptr<ComboAttach>  rootNoteAtt, rangeAtt;
    std::array<std::unique_ptr<ButtonAttach>, 12> noteAtts;

    // Pitch display state
    float displayDetectedHz      = 0.0f;
    float displayTargetHz        = 0.0f;
    float displayCorrectionCents = 0.0f;
    float displayConfidence      = 0.0f;
    int   displayDetectedMidi    = -1;
    int   displayTargetMidi      = -1;
    float displayTargetMidiFloat = -1.0f;
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
    int  currentScaleType() const;
    int  currentRoot() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumHouseVocalTuneEditor)
};
