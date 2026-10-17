#include "PluginEditor.h"
#include <cmath>

using Palette = humtune::Palette;

namespace
{
const juce::StringArray kNoteNames {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// Which piano keys are "black"
constexpr bool isBlackKey (int idx)
{
    return idx == 1 || idx == 3 || idx == 6 || idx == 8 || idx == 10;
}

juce::String targetNoteLabel (int nearestMidi, float exactMidi)
{
    auto name = HumHouseVocalTuneProcessor::midiNoteToName(nearestMidi);
    if (nearestMidi < 0 || ! std::isfinite(exactMidi) || exactMidi < 0.0f)
        return name.isEmpty() ? juce::String("--") : name;

    const int cents = juce::roundToInt((exactMidi - static_cast<float>(nearestMidi)) * 100.0f);
    if (std::abs(cents) >= 1)
    {
        if (cents > 0) name += "+";
        name += juce::String(cents);
        name += juce::String::fromUTF8("¢");
    }
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
HumHouseVocalTuneEditor::HumHouseVocalTuneEditor (HumHouseVocalTuneProcessor& p)
    : AudioProcessorEditor(p), proc(p)
{
    setLookAndFeel(&lnf);

    auto& apvts = proc.getAPVTS();

    // --- Factory program selector ---
    // A compact header control makes the quality-first Studio Max profile discoverable
    // without requiring the DAW to expose legacy program lists. The processor retains
    // responsibility for applying parameter changes and notifying the host.
    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem(proc.getProgramName(i), i + 1);
    // A restored legacy session may contain settings that do not match a factory
    // program. Show that explicitly instead of labelling it "Studio Max".
    presetBox.addItem("Custom / Saved Settings", 1000);
    presetBox.setSelectedId(proc.getCurrentProgram() < 0 ? 1000 : proc.getCurrentProgram() + 1,
                            juce::dontSendNotification);
    presetBox.setTooltip("Factory profiles. Studio Max (Standard) uses the established quality-first shifter, Note Stabilizer off, and Formant Preserve on. Low Latency is experimental and opt-in.");
    presetBox.onChange = [this]()
    {
        const int itemId = presetBox.getSelectedId();
        if (itemId > 0)
            proc.setCurrentProgram(itemId - 1);
    };
    addAndMakeVisible(presetBox);

    autoKeyButton.setTooltip("Estimate the key from recent detected vocal notes, then apply that Major/Minor key.\n"
                             "Play a few notes first; sparse or chromatic performances may be ambiguous.");
    autoKeyButton.onClick = [this]()
    {
        const auto estimate = proc.applyAutoKeyEstimate();
        if (estimate.valid)
        {
            rootNoteBox.setSelectedId(estimate.root + 1, juce::dontSendNotification);
            scaleBox.setSelectedId((estimate.minor ? 1 : 0) + 1, juce::dontSendNotification);
            applyScaleFromControls();
            autoKeyButton.setButtonText(kNoteNames[estimate.root] + juce::String(estimate.minor ? " MIN " : " MAJ ")
                                        + juce::String(juce::roundToInt(estimate.confidence * 100.0f)) + "%");
            autoKeyButton.setTooltip("Applied estimated key. Confidence is a heuristic based on profile fit and runner-up separation, not a probability.");
        }
        else
        {
            autoKeyButton.setButtonText("PLAY MORE");
            autoKeyButton.setTooltip("Not enough distinct, confident vocal pitch history yet. Play/sing a phrase and try again.");
        }
        autoKeyFeedbackTicks = 75;
    };
    addAndMakeVisible(autoKeyButton);

    // --- Knobs ---
    setupKnob(speedKnob, speedLabel, "RETUNE SPEED",
              "How fast the pitch snaps to the note. Turn right for faster.\n0 = instant (hard tune), 100 = slow and natural.");
    speedKnob.getProperties().set("displayScale", 100.0);   // shown as 0..100
    speedKnob.getProperties().set("displayFlip", true);     // fully right = 0 (fastest), fully left = 100 (slowest)

    setupKnob(humanizeKnob, humanizeLabel, "HUMANIZE",
              "Eases off correction on sustained notes.");
    humanizeKnob.getProperties().set("displayScale", 100.0);
    humanizeKnob.getProperties().set("displayUnit", "%");
    setupKnob(flexTuneKnob, flexTuneLabel, "FLEX TUNE",
              "Lets expressive bends and larger pitch excursions pass through more naturally.\n"
              "Low = conventional correction. High = correction is strongest close to the target note.");
    flexTuneKnob.getProperties().set("displayScale", 100.0); // shown as 0..100
    setupKnob(glideKnob, glideLabel, "GLIDE / ms",
              "Smooths movement between target notes independently of Retune Speed.\n"
              "0 ms snaps to the selected target immediately; higher values add portamento.");
    glideKnob.setTextValueSuffix(" ms");
    glideKnob.getProperties().set("displayDecimals", 0);

    setupKnob(referenceFreqKnob, referenceFreqLabel, "PITCH REF / Hz",
              "What frequency the note A4 is tuned to. 440 Hz is the standard.");
    referenceFreqKnob.setTextValueSuffix(" Hz");
    referenceFreqKnob.getProperties().set("displayDecimals", 1);


    setupKnob(formantShiftKnob, formantShiftLabel, "FORMANT / st",
              "Independently shifts vocal tone colour from -4 to +4 semitones.\n"
              "Negative = darker/deeper; positive = brighter. Zero leaves your existing formant setting unchanged.");
    formantShiftKnob.setTextValueSuffix(" st");
    formantShiftKnob.getProperties().set("displayDecimals", 1);

    speedAtt     = std::make_unique<SliderAttach>(apvts, "retune",    speedKnob);
    humanizeAtt  = std::make_unique<SliderAttach>(apvts, "humanize",  humanizeKnob);
    flexTuneAtt  = std::make_unique<SliderAttach>(apvts, "flexTune",  flexTuneKnob);
    glideAtt     = std::make_unique<SliderAttach>(apvts, "glide",     glideKnob);
    referenceFreqAtt = std::make_unique<SliderAttach>(apvts, "referenceFreq", referenceFreqKnob);
    formantShiftAtt = std::make_unique<SliderAttach>(apvts, "formantShift", formantShiftKnob);

    // --- Scale selector: western modes plus optional quarter-tone / Maqam scales ---
    for (int i = 0; i < humtune::ScaleQuantizer::kNumScales; ++i)
        scaleBox.addItem(humtune::ScaleQuantizer::name(i), i + 1);
    scaleBox.setSelectedId(currentScaleType() + 1, juce::dontSendNotification);
    scaleBox.setTooltip("Choose the target-note scale. 24-TET and several Maqam modes use quarter-tone targets.\n"
                        "The piano keys show the nearest semitone classes; manual key edits switch to a custom 12-TET scale.");
    scaleBox.onChange = [this]()
    {
        const int scaleIndex = scaleBox.getSelectedId() - 1;
        if (scaleIndex < 0 || scaleIndex >= humtune::ScaleQuantizer::kNumScales)
            return;
        auto& state = proc.getAPVTS();
        if (scaleIndex < 3)
        {
            if (auto* legacyParam = state.getParameter("scaleType"))
                legacyParam->setValueNotifyingHost(legacyParam->convertTo0to1(static_cast<float>(scaleIndex)));
            if (auto* extensionParam = state.getParameter("scaleExtension"))
                extensionParam->setValueNotifyingHost(extensionParam->convertTo0to1(0.0f));
        }
        else
        {
            // Extension 1 maps to full scale index 3, extension 17 to index 19.
            if (auto* extensionParam = state.getParameter("scaleExtension"))
                extensionParam->setValueNotifyingHost(extensionParam->convertTo0to1(static_cast<float>(scaleIndex - 2)));
        }
        proc.applyScaleToKeys(scaleIndex, currentRoot());
        lastAppliedScale = scaleIndex;
        lastAppliedRoot = currentRoot();
        repaint();
    };
    addAndMakeVisible(scaleBox);

    // --- Toggle buttons ---
    formantBtn.setTooltip("On: keeps the natural character of the voice.\nOff: robotic / chipmunk effect.");
    lowLatencyBtn.setTooltip("On: experimental fixed ~1.9 ms shifter delay for live monitoring.\nOff (default): established, higher-quality Standard shifter with 9-40 ms delay depending on Voice Range.");
    enableBtn.setTooltip("Turns the tuning on and off.");

    addAndMakeVisible(formantBtn);
    addAndMakeVisible(lowLatencyBtn);
    addAndMakeVisible(enableBtn);

    formantAtt    = std::make_unique<ButtonAttach>(apvts, "formantPreserve", formantBtn);
    lowLatencyAtt = std::make_unique<ButtonAttach>(apvts, "lowLatencyMode", lowLatencyBtn);
    enableAtt     = std::make_unique<ButtonAttach>(apvts, "enabled",         enableBtn);

    // --- Voice range: the lowest note the tuner follows. Lower notes need more delay. ---
    rangeBox.addItemList(juce::StringArray { "Bass", "Deep", "Standard", "Fast", "Fastest", "Sub Bass" }, 1);
    rangeBox.setSelectedItemIndex(1, juce::dontSendNotification);
    rangeBox.setTooltip("Voice range: the lowest note the tuner can follow.\n"
                        "Lower notes need more delay.\n"
                        "Bass: 70 Hz and up (about 23 ms)\n"
                        "Deep: 80 Hz and up (about 20 ms)\n"
                        "Standard: 100 Hz and up (about 16 ms)\n"
                        "Fast: 130 Hz and up (about 12 ms)\n"
                        "Fastest: 180 Hz and up (about 9 ms in the standard mode).\n"
                        "Sub Bass: 40 Hz and up (about 40 ms in the standard mode; includes E1).\n"
                        "Experimental Low Latency uses a fixed 1.9 ms shifter budget, but is not yet validated on real vocal recordings or in a DAW.\n"
                        "Notes below the range are not tuned.");
    addAndMakeVisible(rangeBox);
    rangeAtt = std::make_unique<ComboAttach>(apvts, "range", rangeBox);

    // --- Key (root note) ---
    rootNoteBox.addItemList(kNoteNames, 1);
    rootNoteBox.setSelectedItemIndex(0, juce::dontSendNotification);
    rootNoteBox.setTooltip("The key of your song.\nChanging it re-lights the piano keys for the chosen scale.");
    addAndMakeVisible(rootNoteBox);
    rootNoteLabel.setText("KEY", juce::dontSendNotification);
    rootNoteLabel.setJustificationType(juce::Justification::centredRight);
    rootNoteLabel.setColour(juce::Label::textColourId, Palette::textDim);
    addAndMakeVisible(rootNoteLabel);
    rootNoteAtt = std::make_unique<ComboAttach>(apvts, "rootNote", rootNoteBox);

    // Host automation and GUI edits keep the piano projection in sync with the scale/key.
    if (auto* rootParam = apvts.getParameter("rootNote"))
        rootParam->addListener(this);
    if (auto* scaleParam = apvts.getParameter("scaleType"))
        scaleParam->addListener(this);
    lastAppliedScale = currentScaleType();
    lastAppliedRoot = currentRoot();

    // --- Piano keys: lit = the tuner may snap to this note ---
    for (int i = 0; i < 12; ++i)
    {
        auto& btn = noteButtons[static_cast<size_t>(i)];
        btn.setButtonText(kNoteNames[i]);
        btn.setClickingTogglesState(true);
        btn.setToggleState(true, juce::dontSendNotification);
        btn.setTooltip("Click to allow or block this note.\nLit keys are the notes the tuner can snap to.");
        // Tell the look-and-feel to draw this as a piano key (1 = white, 2 = black)
        btn.getProperties().set("pianoKey", isBlackKey(i) ? 2 : 1);
        addAndMakeVisible(btn);

        noteAtts[static_cast<size_t>(i)] =
            std::make_unique<ButtonAttach>(apvts, "note" + juce::String(i), btn);
    }

    heatmapDetected.fill(0.0f);
    heatmapTarget.fill(0.0f);

    // Start at a readable design size; preserve the layout when the host resizes it.
    setResizable(true, true);
    setResizeLimits(720, 544, 1350, 1020);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(kDesignW) / static_cast<double>(kDesignH));
    setSize(juce::roundToInt(static_cast<float>(kDesignW) * 1.0f),
            juce::roundToInt(static_cast<float>(kDesignH) * 1.0f));

    startTimerHz(30);
}

HumHouseVocalTuneEditor::~HumHouseVocalTuneEditor()
{
    if (auto* rootParam = proc.getAPVTS().getParameter("rootNote"))
        rootParam->removeListener(this);
    if (auto* scaleParam = proc.getAPVTS().getParameter("scaleType"))
        scaleParam->removeListener(this);

    setLookAndFeel(nullptr);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::setupKnob (juce::Slider& knob, juce::Label& label,
                                           const juce::String& text, const juce::String& tooltip)
{
    knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    knob.setTooltip(tooltip);
    addAndMakeVisible(knob);

    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, Palette::textDim);
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

int HumHouseVocalTuneEditor::currentScaleType() const
{
    const int extension = static_cast<int>(*proc.getAPVTS().getRawParameterValue("scaleExtension"));
    return extension > 0 ? extension + 2
                         : static_cast<int>(*proc.getAPVTS().getRawParameterValue("scaleType"));
}

int HumHouseVocalTuneEditor::currentRoot() const
{
    return static_cast<int>(*proc.getAPVTS().getRawParameterValue("rootNote"));
}

void HumHouseVocalTuneEditor::applyScaleFromControls()
{
    const int scale = currentScaleType();
    const int root = currentRoot();
    proc.applyScaleToKeys(scale, root);
    lastAppliedScale = scale;
    lastAppliedRoot = root;
    if (scaleBox.getSelectedId() != scale + 1)
        scaleBox.setSelectedId(scale + 1, juce::dontSendNotification);
    repaint();
}

// Named-scale/root changes from the host or UI relight the keyboard. Unchanged values do
// not overwrite a manual key mask; the timer tracks the last applied scale/root pair.
void HumHouseVocalTuneEditor::parameterValueChanged (int, float)
{
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<juce::Component>(this)]
    {
        if (safe != nullptr)
            safe->repaint();
    });
}

void HumHouseVocalTuneEditor::parameterGestureChanged (int, bool gestureIsStarting)
{
    if (gestureIsStarting)
        return;

    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<HumHouseVocalTuneEditor>(this)]
    {
        if (safe != nullptr)
            safe->applyScaleFromControls();
    });
}

// ---------------------------------------------------------------------------
// Timer: refresh display values
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::timerCallback()
{
    // Keep Auto Key feedback transient rather than leaving the estimated key/count
    // permanently in the compact header button.
    if (autoKeyFeedbackTicks > 0 && --autoKeyFeedbackTicks == 0)
    {
        autoKeyButton.setButtonText("AUTO KEY");
        autoKeyButton.setTooltip("Estimate the key from recent detected vocal notes, then apply that Major/Minor key.\n"
                                 "Play a few notes first; sparse or chromatic performances may be ambiguous.");
    }

    const int desiredPresetId = proc.getCurrentProgram() < 0 ? 1000 : proc.getCurrentProgram() + 1;
    if (presetBox.getSelectedId() != desiredPresetId)
        presetBox.setSelectedId(desiredPresetId, juce::dontSendNotification);

    displayDetectedHz      = proc.getDetectedHz();
    displayTargetHz        = proc.getTargetHz();
    displayCorrectionCents = proc.getCorrectionCents();
    displayConfidence      = proc.getConfidence();
    displayDetectedMidi    = proc.getDetectedMidiNote();
    displayTargetMidi      = proc.getTargetMidiNote();
    displayTargetMidiFloat = proc.getTargetMidiFloat();
    displayInputLevel      = proc.getInputLevel();
    displayOutputLevel     = proc.getOutputLevel();

    // Meters: dB scale (-48 dB .. 0 dB), instant attack, smooth release
    auto toMeter = [](float level)
    {
        const float db = juce::Decibels::gainToDecibels(level, -60.0f);
        return juce::jlimit(0.0f, 1.0f, (db + 48.0f) / 48.0f);
    };
    const float inTarget  = toMeter(displayInputLevel);
    const float outTarget = toMeter(displayOutputLevel);
    meterIn  = inTarget  > meterIn  ? inTarget  : meterIn  * 0.88f + inTarget  * 0.12f;
    meterOut = outTarget > meterOut ? outTarget : meterOut * 0.88f + outTarget * 0.12f;

    // Pitch heatmap
    heatmapDetected[static_cast<size_t>(heatmapWriteIdx)] = displayDetectedHz;
    heatmapTarget[static_cast<size_t>(heatmapWriteIdx)]   = displayTargetHz;
    heatmapWriteIdx = (heatmapWriteIdx + 1) % kHeatmapWidth;

    // Reflect host automation and presets. Only rewrite the keyboard when the named
    // scale or root actually changes, so a user's manual key mask is not overwritten.
    const int scaleVal = currentScaleType();
    const int root = currentRoot();
    if (scaleBox.getSelectedId() != scaleVal + 1)
        scaleBox.setSelectedId(scaleVal + 1, juce::dontSendNotification);
    if (scaleVal != lastAppliedScale || root != lastAppliedRoot)
    {
        proc.applyScaleToKeys(scaleVal, root);
        lastAppliedScale = scaleVal;
        lastAppliedRoot = root;
    }

    repaint();
}

// All geometry is in design coordinates; children use the same scale in resized().
void HumHouseVocalTuneEditor::paint(juce::Graphics& g)
{
    g.fillAll(Palette::bgDark);
    g.addTransform(juce::AffineTransform::scale(scaleFactor()));
    auto text = [&](const juce::String& value, float size, juce::Colour colour,
                    int x, int y, int w, int h, juce::Justification align = juce::Justification::centredLeft,
                    bool bold = false)
    {
        g.setFont(juce::Font(juce::FontOptions(size).withStyle(bold ? "Bold" : "Regular")));
        g.setColour(colour);
        g.drawText(value, x, y, w, h, align, false);
    };
    auto panel = [&](float x, float y, float w, float h)
    {
        g.setColour(Palette::bgPanel);
        g.fillRoundedRectangle(x, y, w, h, 9.0f);
        g.setColour(juce::Colour(0xff303639));
        g.drawRoundedRectangle(x, y, w, h, 9.0f, 1.0f);
    };

    // Compact brand and preset toolbar.
    g.setColour(Palette::bgDeep);
    g.fillRect(0, 0, kDesignW, 64);
    g.setColour(Palette::knobTrack);
    g.drawHorizontalLine(63, 0.0f, static_cast<float>(kDesignW));
    g.setColour(Palette::accent);
    g.fillRoundedRectangle(24.0f, 19.0f, 29.0f, 28.0f, 7.0f);
    text("S", 21, Palette::bgDeep, 24, 18, 29, 29, juce::Justification::centred, true);
    text("SkellyTune", 24, Palette::textBright, 65, 12, 180, 30, juce::Justification::centredLeft, true);
    text("HUMHOUSE  /  VOCAL PROCESSOR", 9, Palette::textDim, 66, 41, 208, 12);
    text("PRESET", 10, Palette::textDim, 326, 25, 56, 16, juce::Justification::centredLeft, true);

    panel(24, 84, 220, 288);
    panel(260, 84, 380, 288);
    panel(656, 84, 220, 288);
    text("01  /  CORRECTION", 10, Palette::textDim, 42, 98, 184, 16, juce::Justification::centredLeft, true);
    text("02  /  CHARACTER", 10, Palette::textDim, 674, 98, 184, 16, juce::Justification::centredLeft, true);

    const bool tracking = displayDetectedHz > 35.0f && displayConfidence > 0.3f;
    const auto note = HumHouseVocalTuneProcessor::midiNoteToName(displayDetectedMidi);
    const auto target = targetNoteLabel(displayTargetMidi, displayTargetMidiFloat);
    text("PITCH MONITOR", 10, Palette::textDim, 280, 99, 170, 15, juce::Justification::centredLeft, true);
    g.setColour(tracking ? Palette::accent : Palette::textDim.withAlpha(0.4f));
    g.fillEllipse(550, 104, 5, 5);
    text(tracking ? "TRACKING" : "LISTENING", 9, Palette::textDim, 561, 98, 62, 16);
    text("INPUT NOTE", 10, Palette::textDim, 282, 128, 138, 16, juce::Justification::centred, true);
    text("TARGET NOTE", 10, Palette::textDim, 482, 128, 138, 16, juce::Justification::centred, true);
    text(tracking ? note : "--", 52, Palette::textBright, 282, 144, 138, 63, juce::Justification::centred, true);
    text(tracking ? target : "--", 44, Palette::accent, 482, 144, 138, 63, juce::Justification::centred, true);
    g.setColour(Palette::textDim);
    g.drawArrow(juce::Line<float>(438, 174, 462, 174), 1.0f, 6.0f, 6.0f);
    text(tracking ? juce::String(displayDetectedHz, 1) + " Hz" : "Waiting for vocal", 11, Palette::textDim,
         282, 207, 138, 18, juce::Justification::centred);
    text(tracking ? juce::String(displayTargetHz, 1) + " Hz" : "Follow selected scale", 11, Palette::textDim,
         482, 207, 138, 18, juce::Justification::centred);

    // Correction ruler: signed cents with a fixed, readable zero point.
    const float centre = 450.0f;
    g.setColour(Palette::knobTrack);
    g.fillRoundedRectangle(294, 243, 312, 4, 2);
    for (int i = -2; i <= 2; ++i)
    {
        const float x = centre + static_cast<float>(i) * 78.0f;
        g.setColour(i == 0 ? Palette::textDim : Palette::knobTrack);
        g.drawLine(x, 238, x, 252, 1);
    }
    if (tracking)
    {
        const float delta = juce::jlimit(-100.0f, 100.0f, displayCorrectionCents) * 1.56f;
        g.setColour(Palette::accent);
        g.fillRoundedRectangle(juce::jmin(centre, centre + delta), 243, juce::jmax(2.0f, std::abs(delta)), 4, 2);
        g.fillEllipse(centre + delta - 3.5f, 241.5f, 7, 7);
    }
    text("-100", 9, Palette::textDim, 289, 255, 40, 14);
    text("+100", 9, Palette::textDim, 571, 255, 40, 14, juce::Justification::centredRight);
    text(tracking ? (displayCorrectionCents >= 0 ? "+" : "") + juce::String(juce::roundToInt(displayCorrectionCents)) + " ct correction"
                  : "Pitch correction", 10, Palette::textDim, 365, 255, 170, 14, juce::Justification::centred);

    // Log-frequency pitch history; gaps remain gaps when the detector is unvoiced.
    const juce::Rectangle<float> history(280, 290, 340, 51);
    g.setColour(Palette::bgDeep);
    g.fillRoundedRectangle(history, 4);
    for (int i = 1; i < 4; ++i)
    {
        g.setColour(Palette::knobTrack.withAlpha(0.4f));
        const float y = history.getY() + static_cast<float>(i) * history.getHeight() / 4;
        g.drawLine(history.getX(), y, history.getRight(), y, 0.5f);
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        juce::Path line;
        bool connected = false;
        for (int i = 0; i < kHeatmapWidth; ++i)
        {
            const auto idx = static_cast<size_t>((heatmapWriteIdx + i) % kHeatmapWidth);
            const float hz = pass == 0 ? heatmapDetected[idx] : heatmapTarget[idx];
            if (!std::isfinite(hz) || hz < 35.0f) { connected = false; continue; }
            const float norm = juce::jlimit(0.0f, 1.0f, std::log2(hz / 35.0f) / std::log2(1500.0f / 35.0f));
            const float x = history.getX() + static_cast<float>(i) / static_cast<float>(kHeatmapWidth - 1) * history.getWidth();
            const float y = history.getBottom() - 4 - norm * (history.getHeight() - 8);
            if (connected) line.lineTo(x, y); else line.startNewSubPath(x, y);
            connected = true;
        }
        g.setColour(pass == 0 ? Palette::heatCold.withAlpha(0.7f) : Palette::heatWarm);
        g.strokePath(line, juce::PathStrokeType(pass == 0 ? 1.0f : 1.5f));
    }
    text("INPUT", 9, Palette::heatCold, 280, 347, 46, 14);
    text("TARGET", 9, Palette::heatWarm, 330, 347, 50, 14);
    const auto latency = proc.getSampleRate() > 0 ? juce::String(1000.0 * proc.getLatencySamples() / proc.getSampleRate(), 1) + " ms" : "-- ms";
    text("LATENCY  " + latency, 9, Palette::textDim, 470, 347, 150, 14, juce::Justification::centredRight);

    auto meter = [&](const juce::String& label, float value, float level, int x)
    {
        text(label, 9, Palette::textDim, x, 351, 26, 14, juce::Justification::centredLeft, true);
        g.setColour(Palette::bgDeep);
        g.fillRoundedRectangle(static_cast<float>(x + 28), 356, 109, 3, 1.5f);
        g.setColour(level >= 1.0f ? Palette::meterRed : Palette::accent);
        g.fillRoundedRectangle(static_cast<float>(x + 28), 356, 109 * value, 3, 1.5f);
        text(level > 0.0001f ? juce::String(juce::Decibels::gainToDecibels(level), 0) + " dB" : "-inf", 9,
             Palette::textDim, x + 141, 351, 40, 14, juce::Justification::centredRight);
    };
    meter("IN", meterIn, displayInputLevel, 42);
    meter("OUT", meterOut, displayOutputLevel, 674);

    panel(24, 392, 852, 94);
    text("KEY", 10, Palette::textDim, 42, 407, 60, 14, juce::Justification::centredLeft, true);
    text("SCALE", 10, Palette::textDim, 126, 407, 200, 14, juce::Justification::centredLeft, true);
    text("VOICE RANGE", 10, Palette::textDim, 368, 407, 146, 14, juce::Justification::centredLeft, true);
    text("PROCESSING", 10, Palette::textDim, 558, 407, 290, 14, juce::Justification::centredLeft, true);
    text("TARGET NOTES", 10, Palette::textDim, 25, 505, 200, 16, juce::Justification::centredLeft, true);
    text("Click a key to include or exclude a note", 11, Palette::textDim, 495, 505, 380, 16, juce::Justification::centredRight);
    g.setColour(Palette::bgDeep);
    g.fillRoundedRectangle(24, 532, 852, 112, 6);

    g.setColour(Palette::knobTrack);
    g.drawHorizontalLine(655, 24, 876);
    text("HUMHOUSE", 10, Palette::textDim, 25, 660, 115, 15, juce::Justification::centredLeft, true);
    const bool enabled = enableBtn.getToggleState();
    text(!enabled ? "TUNING BYPASSED" : lowLatencyBtn.getToggleState() ? "EXPERIMENTAL LOW LATENCY" : "STANDARD PROCESSING",
         9, enabled ? Palette::accent : Palette::textDim, 570, 660, 305, 15, juce::Justification::centredRight);
}

void HumHouseVocalTuneEditor::paintOverChildren(juce::Graphics& g)
{
    const float s = scaleFactor();
    g.addTransform(juce::AffineTransform::scale(s));
    auto marker = [&](int midi, bool target)
    {
        if (midi < 0 || displayConfidence <= 0.3f || displayDetectedHz <= 35.0f) return;
        const auto key = noteButtons[static_cast<size_t>((midi % 12 + 12) % 12)].getBounds().toFloat() * (1.0f / s);
        const float x = key.getCentreX(), y = key.getBottom() - 43;
        g.setColour(target ? Palette::accent : Palette::heatCold);
        if (target) g.drawEllipse(x - 6, y - 6, 12, 12, 1.5f);
        else g.fillEllipse(x - 3, y - 3, 6, 6);
    };
    marker(displayTargetMidi, true);
    marker(displayDetectedMidi, false);
}

void HumHouseVocalTuneEditor::resized()
{
    const float s = scaleFactor();
    lnf.setScale(s);
    auto R = [s](int x, int y, int w, int h)
    {
        return juce::Rectangle<int>(juce::roundToInt(static_cast<float>(x) * s), juce::roundToInt(static_cast<float>(y) * s),
                                   juce::roundToInt(static_cast<float>(w) * s), juce::roundToInt(static_cast<float>(h) * s));
    };
    auto place = [&](juce::Slider& knob, juce::Label& label, int x, int y, int size, int labelWidth)
    {
        knob.setBounds(R(x, y, size, size));
        label.setBounds(R(x + (size - labelWidth) / 2, y + size + 4, labelWidth, 16));
        label.setFont(juce::Font(juce::FontOptions(10.0f * s).withStyle("Bold")));
    };
    presetBox.setBounds(R(384, 17, 238, 32));
    autoKeyButton.setBounds(R(638, 17, 116, 32));
    enableBtn.setBounds(R(770, 17, 106, 32));
    place(speedKnob, speedLabel, 67, 117, 134, 174);
    place(humanizeKnob, humanizeLabel, 699, 117, 134, 174);
    place(flexTuneKnob, flexTuneLabel, 62, 278, 54, 90);
    place(glideKnob, glideLabel, 151, 278, 54, 80);
    place(referenceFreqKnob, referenceFreqLabel, 693, 278, 58, 90);
    place(formantShiftKnob, formantShiftLabel, 784, 278, 54, 90);
    rootNoteLabel.setVisible(false);
    rootNoteBox.setBounds(R(42, 429, 70, 36));
    scaleBox.setBounds(R(126, 429, 222, 36));
    rangeBox.setBounds(R(368, 429, 166, 36));
    formantBtn.setBounds(R(558, 429, 125, 36));
    lowLatencyBtn.setBounds(R(697, 429, 161, 36));

    constexpr int whiteNotes[] {0, 2, 4, 5, 7, 9, 11};
    constexpr int blackNotes[] {1, 3, 6, 8, 10};
    constexpr int blackPositions[] {0, 1, 3, 4, 5};
    for (int i = 0; i < 7; ++i)
        noteButtons[static_cast<size_t>(whiteNotes[i])].setBounds(R(29 + i * 120, 537, 118, 102));
    for (int i = 0; i < 5; ++i)
    {
        auto& key = noteButtons[static_cast<size_t>(blackNotes[i])];
        key.setBounds(R(29 + blackPositions[i] * 120 + 84, 537, 70, 62));
        key.toFront(false);
    }
}
