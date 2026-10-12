#include "PluginEditor.h"
#include "Artwork.h"

using Palette = humtune::Palette;

namespace
{
const juce::StringArray kNoteNames {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// Which piano keys are "black"
constexpr bool isBlackKey (int idx)
{
    return idx == 1 || idx == 3 || idx == 6 || idx == 8 || idx == 10;
}

// Radar geometry, in design coordinates (780 x 620)
constexpr float kCx = 282.0f;
constexpr float kCy = 206.0f;
constexpr float kR  = 128.0f;

// Shrinks an image in steps of at most 2x, which keeps the result smooth
juce::Image scaledImage (const juce::Image& src, int w, int h)
{
    if (! src.isValid() || w < 1 || h < 1)
        return {};

    juce::Image cur = src;
    while (cur.getWidth() > 2 * w || cur.getHeight() > 2 * h)
        cur = cur.rescaled(juce::jmax(w, cur.getWidth() / 2),
                           juce::jmax(h, cur.getHeight() / 2),
                           juce::Graphics::highResamplingQuality);

    return cur.rescaled(w, h, juce::Graphics::highResamplingQuality);
}
} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
HumHouseVocalTuneEditor::HumHouseVocalTuneEditor (HumHouseVocalTuneProcessor& p)
    : AudioProcessorEditor(p), proc(p)
{
    setLookAndFeel(&lnf);

    // Decode the embedded artwork once (see Artwork.h)
    skullSolidImg = juce::ImageFileFormat::loadFrom(humtune::art::skullSolid,
                                                     static_cast<size_t>(humtune::art::skullSolidSize));
    skullHoloImg  = juce::ImageFileFormat::loadFrom(humtune::art::skullHolo,
                                                     static_cast<size_t>(humtune::art::skullHoloSize));
    armImg        = juce::ImageFileFormat::loadFrom(humtune::art::armHand,
                                                     static_cast<size_t>(humtune::art::armHandSize));

    addAndMakeVisible(armLeftView);
    addAndMakeVisible(armRightView);
    addAndMakeVisible(skullView);

    auto& apvts = proc.getAPVTS();

    // --- Knobs ---
    setupKnob(speedKnob, speedLabel, "RETUNE SPEED",
              "How fast the pitch snaps to the note. Turn right for faster.\n0 = instant (hard tune), 100 = slow and natural.");
    speedKnob.getProperties().set("displayScale", 100.0);   // shown as 0..100
    speedKnob.getProperties().set("displayFlip", true);     // fully right = 0 (fastest), fully left = 100 (slowest)

    setupKnob(humanizeKnob, humanizeLabel, "HUMANIZE",
              "Keeps some of the natural pitch movement of the voice.");
    setupKnob(inputGainKnob, inputGainLabel, "IN GAIN", "Level going into the tuner (dB).");
    setupKnob(outputGainKnob, outputGainLabel, "OUT GAIN", "Level coming out of the plugin (dB).");
    setupKnob(referenceFreqKnob, referenceFreqLabel, "PITCH REF",
              "What frequency the note A4 is tuned to. 440 Hz is the standard.");
    referenceFreqKnob.setTextValueSuffix(" Hz");
    referenceFreqKnob.getProperties().set("hideValue", true);   // the value is shown next to the knob

    speedAtt     = std::make_unique<SliderAttach>(apvts, "retune",    speedKnob);
    humanizeAtt  = std::make_unique<SliderAttach>(apvts, "humanize",  humanizeKnob);
    inputGainAtt = std::make_unique<SliderAttach>(apvts, "inputGain", inputGainKnob);
    outputGainAtt= std::make_unique<SliderAttach>(apvts, "outputGain",outputGainKnob);
    referenceFreqAtt = std::make_unique<SliderAttach>(apvts, "referenceFreq", referenceFreqKnob);

    // --- Scale buttons: pick a scale and the piano keys light up for it ---
    auto makeScaleBtn = [this](juce::TextButton& btn, int scaleIdx, const juce::String& tip)
    {
        btn.setClickingTogglesState(true);
        btn.setRadioGroupId(1001);
        btn.setToggleState(scaleIdx == 0, juce::dontSendNotification);
        btn.setTooltip(tip);
        btn.onClick = [this, scaleIdx]()
        {
            if (auto* param = proc.getAPVTS().getParameter("scaleType"))
                param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(scaleIdx)));
            proc.applyScaleToKeys(scaleIdx, currentRoot());
            repaint();
        };
        addAndMakeVisible(btn);
    };
    makeScaleBtn(majorBtn, 0, "Major scale in the chosen key.\nLights the matching piano keys.");
    makeScaleBtn(minorBtn, 1, "Minor scale in the chosen key.\nLights the matching piano keys.");
    makeScaleBtn(chromBtn, 2, "Every note is allowed.");

    // --- Toggle buttons ---
    stabilizerBtn.setTooltip("Holds a note a little longer, so vibrato\ndoes not make the tuning jump between notes.");
    formantBtn.setTooltip("On: keeps the natural character of the voice.\nOff: robotic / chipmunk effect.");
    enableBtn.setTooltip("Turns the tuning on and off.");

    addAndMakeVisible(stabilizerBtn);
    addAndMakeVisible(formantBtn);
    addAndMakeVisible(enableBtn);

    stabilizerAtt = std::make_unique<ButtonAttach>(apvts, "noteStabilizer",  stabilizerBtn);
    formantAtt    = std::make_unique<ButtonAttach>(apvts, "formantPreserve", formantBtn);
    enableAtt     = std::make_unique<ButtonAttach>(apvts, "enabled",         enableBtn);

    // --- Voice range: the lowest note the tuner follows. Lower notes need more delay. ---
    rangeBox.addItemList(juce::StringArray { "Bass - 23 ms", "Deep - 20 ms", "Standard - 16 ms",
                                              "Fast - 12 ms", "Fastest - 9 ms" }, 1);
    rangeBox.setSelectedItemIndex(1, juce::dontSendNotification);
    rangeBox.setTooltip("Voice range: the lowest note the tuner can follow.\n"
                        "Lower notes need more delay.\n"
                        "Bass: 70 Hz and up (about 23 ms)\n"
                        "Deep: 80 Hz and up (about 20 ms)\n"
                        "Standard: 100 Hz and up (about 16 ms)\n"
                        "Fast: 130 Hz and up (about 12 ms)\n"
                        "Fastest: 180 Hz and up (about 9 ms)\n"
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

    // When the user changes the Key, the piano keys follow (see parameterGestureChanged)
    if (auto* rootParam = apvts.getParameter("rootNote"))
        rootParam->addListener(this);

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

    // The window starts at 75% of the design size and can be resized from the bottom-right corner
    setResizable(true, true);
    setResizeLimits(468, 372, 1170, 930);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(kDesignW) / static_cast<double>(kDesignH));
    setSize(juce::roundToInt(static_cast<float>(kDesignW) * 0.75f),
            juce::roundToInt(static_cast<float>(kDesignH) * 0.75f));

    startTimerHz(30);
}

HumHouseVocalTuneEditor::~HumHouseVocalTuneEditor()
{
    if (auto* rootParam = proc.getAPVTS().getParameter("rootNote"))
        rootParam->removeListener(this);

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
    return static_cast<int>(*proc.getAPVTS().getRawParameterValue("scaleType"));
}

int HumHouseVocalTuneEditor::currentRoot() const
{
    return static_cast<int>(*proc.getAPVTS().getRawParameterValue("rootNote"));
}

void HumHouseVocalTuneEditor::applyScaleFromControls()
{
    proc.applyScaleToKeys(currentScaleType(), currentRoot());
    repaint();
}

// The Key can also be changed by the host (automation, presets): only a change made with
// the mouse (a "gesture") re-lights the piano keys, so loading a session keeps its own keys.
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
// Artwork: copies at the exact pixel size they are shown at (rebuilt when resized)
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::rebuildScaledArt()
{
    const float s = scaleFactor();

    // Skull + crossbones, in the middle of the radar
    if (skullSolidImg.isValid())
    {
        const int skullW = juce::jmax(1, juce::roundToInt(170.0f * s));
        const int skullH = juce::jmax(1, juce::roundToInt(static_cast<float>(skullW)
                                * static_cast<float>(skullSolidImg.getHeight())
                                / static_cast<float>(skullSolidImg.getWidth())));
        skullView.solid = scaledImage(skullSolidImg, skullW, skullH);
        skullView.holo  = scaledImage(skullHoloImg,  skullW, skullH);
    }

    // Arm + hand: one copy, and a mirrored copy for the right-hand side
    if (armImg.isValid())
    {
        const int armH = juce::jmax(1, juce::roundToInt(270.0f * s));
        const int armW = juce::jmax(1, juce::roundToInt(static_cast<float>(armH)
                                * static_cast<float>(armImg.getWidth())
                                / static_cast<float>(armImg.getHeight())));
        armLeftView.image = scaledImage(armImg, armW, armH);

        juce::Image flipped(juce::Image::ARGB, armW, armH, true);
        {
            juce::Graphics fg(flipped);
            fg.addTransform(juce::AffineTransform::scale(-1.0f, 1.0f).translated(static_cast<float>(armW), 0.0f));
            fg.drawImageAt(armLeftView.image, 0, 0);
        }
        armRightView.image = flipped;
    }
}

// ---------------------------------------------------------------------------
// Timer: refresh display values
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::timerCallback()
{
    displayDetectedHz      = proc.getDetectedHz();
    displayTargetHz        = proc.getTargetHz();
    displayCorrectionCents = proc.getCorrectionCents();
    displayConfidence      = proc.getConfidence();
    displayDetectedMidi    = proc.getDetectedMidiNote();
    displayTargetMidi      = proc.getTargetMidiNote();
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

    // A scale button is lit only while the piano keys exactly match it (otherwise: custom)
    const int scaleVal = currentScaleType();
    const int root     = currentRoot();
    bool keysMatch = true;
    for (int i = 0; i < 12; ++i)
        if (noteButtons[static_cast<size_t>(i)].getToggleState()
                != HumHouseVocalTuneProcessor::isNoteInScale(scaleVal, root, i))
            keysMatch = false;

    majorBtn.setToggleState(keysMatch && scaleVal == 0, juce::dontSendNotification);
    minorBtn.setToggleState(keysMatch && scaleVal == 1, juce::dontSendNotification);
    chromBtn.setToggleState(keysMatch && scaleVal == 2, juce::dontSendNotification);

    // The skull lights up with the singer's level (fast attack, quick release)
    const float glowTarget = juce::jlimit(0.0f, 1.0f, displayInputLevel * 1.6f);
    skullView.glow += 0.35f * (glowTarget - skullView.glow);
    skullView.repaint();

    repaint();
}

// ---------------------------------------------------------------------------
// Paint (everything is drawn in design coordinates and scaled)
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::paint (juce::Graphics& g)
{
    const float s = scaleFactor();
    const float twoPi = juce::MathConstants<float>::twoPi;

    // Background gradient (in real pixels)
    juce::ColourGradient bgGrad(Palette::bgDark, 0.0f, 0.0f,
                                 Palette::bgDeep, 0.0f, static_cast<float>(getHeight()),
                                 false);
    g.setGradientFill(bgGrad);
    g.fillAll();

    g.addTransform(juce::AffineTransform::scale(s));

    auto caption = [&](const juce::String& text, int x, int y, int w,
                       juce::Justification just = juce::Justification::centredLeft)
    {
        g.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
        g.setColour(Palette::textDim);
        g.drawText(text, x, y, w, 14, just, false);
    };

    auto panel = [&](float x, float y, float w, float h, float radius, juce::Colour fill, juce::Colour edge)
    {
        g.setColour(fill);
        g.fillRoundedRectangle(x, y, w, h, radius);
        g.setColour(edge);
        g.drawRoundedRectangle(x, y, w, h, radius, 1.0f);
    };

    // Outer frame
    g.setColour(Palette::accentDim);
    g.drawRect(0, 0, kDesignW, kDesignH, 2);
    g.setColour(Palette::accent.withAlpha(0.35f));
    g.drawRect(3, 3, kDesignW - 6, kDesignH - 6, 1);

    // --- Title bar ---
    g.setColour(Palette::bgPanel);
    g.fillRect(0, 0, kDesignW, 44);
    g.setColour(Palette::accentDim);
    g.drawLine(0.0f, 44.0f, static_cast<float>(kDesignW), 44.0f, 1.5f);

    {
        auto logo = juce::Rectangle<float>(12.0f, 6.0f, 32.0f, 32.0f);
        g.setColour(Palette::accentGlow);
        g.drawEllipse(logo.expanded(2.0f), 3.0f);
        g.setColour(Palette::accentBright);
        g.drawEllipse(logo, 1.5f);
        g.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));
        g.drawText("ST", logo.toNearestInt(), juce::Justification::centred, false);
    }

    {
        auto titleArea = juce::Rectangle<int>(60, 0, kDesignW - 60 - 130, 44);
        const juce::String title = "SkellyTune";
        g.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));

        g.setColour(Palette::accent.withAlpha(0.16f));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx != 0 || dy != 0)
                    g.drawText(title, titleArea.translated(dx, dy), juce::Justification::centred, false);

        g.setColour(Palette::accentBright);
        g.drawText(title, titleArea, juce::Justification::centred, false);

        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.setColour(Palette::textDim);
        g.drawText("v1.0.0", kDesignW - 190, 0, 66, 44, juce::Justification::centredRight, false);
    }

    // --- Main panel and its two side panels ---
    panel(8.0f, 52.0f, 764.0f, 310.0f, 8.0f, Palette::bgPanel.withAlpha(0.9f), Palette::accentDim);

    panel(12.0f, 92.0f, 64.0f, 190.0f, 6.0f, Palette::bgSection.withAlpha(0.8f), Palette::accentDeep);
    caption("LEVELS", 12, 98, 64, juce::Justification::centred);

    panel(488.0f, 56.0f, 280.0f, 298.0f, 10.0f, Palette::bgSection.withAlpha(0.6f), Palette::accentDeep);
    caption("TUNING", 502, 63, 120);

    // Corner brackets on the main panel
    {
        g.setColour(Palette::accentBright);
        const float L = 14.0f;
        const float x0 = 8.0f, y0 = 52.0f, x1 = 772.0f, y1 = 362.0f;
        g.drawLine(x0, y0 + L, x0, y0, 2.0f);   g.drawLine(x0, y0, x0 + L, y0, 2.0f);
        g.drawLine(x1 - L, y0, x1, y0, 2.0f);   g.drawLine(x1, y0, x1, y0 + L, 2.0f);
        g.drawLine(x0, y1 - L, x0, y1, 2.0f);   g.drawLine(x0, y1, x0 + L, y1, 2.0f);
        g.drawLine(x1 - L, y1, x1, y1, 2.0f);   g.drawLine(x1, y1 - L, x1, y1, 2.0f);
    }

    // --- Radar (the skull and the arms are separate child components on top of this) ---
    {
        const float cx = kCx, cy = kCy, R = kR;

        juce::ColourGradient glow(Palette::accent.withAlpha(0.18f), cx, cy,
                                   Palette::accent.withAlpha(0.0f),  cx, cy - R - 14.0f, true);
        g.setGradientFill(glow);
        g.fillEllipse(cx - R - 14.0f, cy - R - 14.0f, (R + 14.0f) * 2.0f, (R + 14.0f) * 2.0f);

        g.setColour(Palette::accentDim);
        g.drawEllipse(cx - R, cy - R, R * 2.0f, R * 2.0f, 2.0f);

        for (int i = 0; i < 90; ++i)
        {
            const float a = static_cast<float>(i) / 90.0f * twoPi;
            const bool major = (i % 5 == 0);
            const float r1 = R - 8.0f;
            const float r2 = R - (major ? 19.0f : 14.0f);
            g.setColour(major ? Palette::accent.withAlpha(0.8f) : Palette::accentDim.withAlpha(0.7f));
            g.drawLine(cx + r1 * std::sin(a), cy - r1 * std::cos(a),
                       cx + r2 * std::sin(a), cy - r2 * std::cos(a),
                       major ? 1.6f : 1.0f);
        }

        g.setColour(Palette::accentDeep);
        for (float inset : { 30.0f, 62.0f, 94.0f })
            g.drawEllipse(cx - (R - inset), cy - (R - inset), (R - inset) * 2.0f, (R - inset) * 2.0f, 1.0f);

        g.setColour(Palette::accent.withAlpha(0.55f));
        g.drawLine(cx - R - 8.0f, cy, cx + R + 8.0f, cy, 1.0f);
        g.drawLine(cx, cy - R - 8.0f, cx, cy + R + 8.0f, 1.0f);

        {
            juce::Path tri;
            tri.addTriangle(cx - 7.0f, cy - R - 14.0f, cx + 7.0f, cy - R - 14.0f, cx, cy - R - 3.0f);
            tri.addTriangle(cx - 7.0f, cy + R + 14.0f, cx + 7.0f, cy + R + 14.0f, cx, cy + R + 3.0f);
            g.setColour(Palette::accentBright);
            g.fillPath(tri);
        }

        {
            const float sweep = static_cast<float>(juce::Time::getMillisecondCounter() % 4000u) / 4000.0f * twoPi;
            g.setColour(Palette::accent.withAlpha(0.22f));
            g.drawLine(cx, cy, cx + (R - 6.0f) * std::sin(sweep), cy - (R - 6.0f) * std::cos(sweep), 1.5f);
        }

        if (displayConfidence > 0.01f)
        {
            const float half = juce::jlimit(0.0f, 1.0f, displayConfidence) * juce::MathConstants<float>::pi;
            juce::Path conf;
            conf.addCentredArc(cx, cy, R - 2.0f, R - 2.0f, 0.0f, -half, half, true);
            g.setColour(Palette::accent.withAlpha(0.22f));
            g.strokePath(conf, juce::PathStrokeType(11.0f));
            g.setColour(Palette::accentBright);
            g.strokePath(conf, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        {
            const float ro = R - 62.0f;
            juce::ColourGradient orbGrad(Palette::accent.withAlpha(0.30f), cx, cy,
                                          Palette::bgDark.withAlpha(0.9f), cx, cy - ro, true);
            g.setGradientFill(orbGrad);
            g.fillEllipse(cx - ro, cy - ro, ro * 2.0f, ro * 2.0f);
        }
    }

    // --- Pitch history strip ---
    const juce::Rectangle<int> heatArea(20, 406, 574, 34);
    g.setColour(Palette::bgSection);
    g.fillRoundedRectangle(heatArea.toFloat(), 4.0f);
    g.setColour(Palette::accentDeep);
    g.drawRoundedRectangle(heatArea.toFloat(), 4.0f, 1.0f);

    for (int i = 0; i < kHeatmapWidth; ++i)
    {
        const int idx = (heatmapWriteIdx + i) % kHeatmapWidth;
        const float det = heatmapDetected[static_cast<size_t>(idx)];
        const float tgt = heatmapTarget[static_cast<size_t>(idx)];

        if (det < 60.0f) continue;

        const float x = static_cast<float>(heatArea.getX())
                      + static_cast<float>(i) / static_cast<float>(kHeatmapWidth)
                      * static_cast<float>(heatArea.getWidth());

        // Map Hz to Y (log scale, 60..1500 Hz)
        auto hzToY = [&](float hz) -> float
        {
            float norm = (std::log2(hz) - std::log2(60.0f))
                       / (std::log2(1500.0f) - std::log2(60.0f));
            norm = juce::jlimit(0.0f, 1.0f, norm);
            return static_cast<float>(heatArea.getBottom()) - 3.0f
                 - norm * static_cast<float>(heatArea.getHeight() - 6);
        };

        g.setColour(Palette::heatCold.withAlpha(0.7f));
        g.fillRect(x, hzToY(det), 3.0f, 2.0f);

        if (tgt > 60.0f)
        {
            g.setColour(Palette::heatWarm.withAlpha(0.9f));
            g.fillRect(x, hzToY(tgt), 3.0f, 2.0f);
        }
    }

    // Legend (with a backing so it stays readable on top of the pitch line)
    {
        g.setColour(Palette::bgSection.withAlpha(0.9f));
        g.fillRoundedRectangle(26.0f, 408.0f, 122.0f, 16.0f, 4.0f);
        g.setFont(juce::Font(juce::FontOptions(11.5f)));
        g.setColour(Palette::heatCold);
        g.fillEllipse(30.0f, 412.0f, 6.0f, 6.0f);
        g.setColour(Palette::textDim);
        g.drawText("Sung", 40, 409, 40, 12, juce::Justification::centredLeft, false);
        g.setColour(Palette::heatWarm);
        g.fillEllipse(82.0f, 412.0f, 6.0f, 6.0f);
        g.setColour(Palette::textDim);
        g.drawText("Target", 92, 409, 50, 12, juce::Justification::centredLeft, false);
    }

    // --- Monitor panel: meters, readouts, reference ---
    panel(604.0f, 406.0f, 168.0f, 198.0f, 8.0f, Palette::bgPanel, Palette::accentDim);
    caption("MONITOR", 614, 411, 100);

    auto drawMeter = [&](const juce::String& label, float value, float y)
    {
        g.setFont(juce::Font(juce::FontOptions(11.5f)));
        g.setColour(Palette::textDim);
        g.drawText(label, 614, static_cast<int>(y) - 3, 30, 14, juce::Justification::centredLeft, false);

        const float mx = 646.0f, mw = 118.0f, mh = 8.0f;
        g.setColour(Palette::bgSection);
        g.fillRoundedRectangle(mx, y, mw, mh, 3.0f);

        juce::ColourGradient grad(Palette::meterGreen, mx, 0.0f, Palette::meterRed, mx + mw, 0.0f, false);
        grad.addColour(0.7, Palette::meterYellow);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(mx, y, mw * juce::jlimit(0.0f, 1.0f, value), mh, 3.0f);
    };
    drawMeter("IN",  meterIn,  430.0f);
    drawMeter("OUT", meterOut, 444.0f);

    auto row = [&](const juce::String& label, const juce::String& value, int y)
    {
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.setColour(Palette::textDim);
        g.drawText(label, 614, y, 62, 16, juce::Justification::centredLeft, false);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.setColour(Palette::textValue);
        g.drawText(value, 676, y, 88, 16, juce::Justification::centredRight, false);
    };

    const juce::String detNote = HumHouseVocalTuneProcessor::midiNoteToName(displayDetectedMidi);
    const juce::String tgtNote = HumHouseVocalTuneProcessor::midiNoteToName(displayTargetMidi);
    const double latencyMs = 1000.0 * static_cast<double>(proc.getLatencySamples())
                           / juce::jmax(1.0, proc.getSampleRate());

    row("Detected",   detNote.isEmpty() ? juce::String("--")
                                        : detNote + "  " + juce::String(juce::roundToInt(displayDetectedHz)) + " Hz", 462);
    row("Target",     tgtNote.isEmpty() ? juce::String("--")
                                        : tgtNote + "  " + juce::String(juce::roundToInt(displayTargetHz)) + " Hz", 480);
    row("Correction", (detNote.isEmpty() ? juce::String("--")
                                         : (displayCorrectionCents >= 0.0f ? "+" : "")
                                           + juce::String(juce::roundToInt(displayCorrectionCents)) + " cents"), 498);
    row("Latency",    juce::String(latencyMs, 1) + " ms", 516);

    g.setColour(Palette::accentDeep);
    g.drawLine(614.0f, 538.0f, 762.0f, 538.0f, 1.0f);

    {
        const float refHz = *proc.getAPVTS().getRawParameterValue("referenceFreq");
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.setColour(Palette::textDim);
        g.drawText("A4 =", 614, 556, 40, 16, juce::Justification::centredLeft, false);
        g.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));
        g.setColour(Palette::textValue);
        g.drawText(juce::String(refHz, 1) + " Hz", 614, 572, 90, 18, juce::Justification::centredLeft, false);
    }

    // --- Piano keyboard backing ---
    panel(20.0f, 446.0f, 574.0f, 158.0f, 6.0f, Palette::bgPanel, Palette::accentDim);

}

// ---------------------------------------------------------------------------
// Things drawn on top of the child components
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::paintOverChildren (juce::Graphics& g)
{
    const float s = scaleFactor();
    g.addTransform(juce::AffineTransform::scale(s));

    const float cx = kCx, cy = kCy, R = kR;

    // Correction needle
    if (displayDetectedHz > 60.0f && displayConfidence > 0.3f)
    {
        const float normCorrection = juce::jlimit(-1.0f, 1.0f, displayCorrectionCents / 100.0f);
        const float angle = normCorrection * juce::MathConstants<float>::halfPi;
        const float indicatorLen = (R - 62.0f) * 0.9f * displayConfidence;
        const float ix = cx + indicatorLen * std::sin(angle);
        const float iy = cy - indicatorLen * std::cos(angle);

        g.setColour(Palette::accent.withAlpha(0.3f));
        g.drawLine(cx, cy, ix, iy, 8.0f);
        g.setColour(Palette::accentBright);
        g.drawLine(cx, cy, ix, iy, 3.0f);
        g.fillEllipse(ix - 5.0f, iy - 5.0f, 10.0f, 10.0f);
    }

    // Detected note, under the skull (between the crossbones)
    {
        juce::String noteText = HumHouseVocalTuneProcessor::midiNoteToName(displayDetectedMidi);
        if (noteText.isEmpty()) noteText = "--";

        const juce::Rectangle<int> noteRect(static_cast<int>(cx) - 50, static_cast<int>(cy) + 46, 100, 38);
        g.setFont(juce::Font(juce::FontOptions(32.0f).withStyle("Bold")));
        g.setColour(Palette::accent.withAlpha(0.16f));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx != 0 || dy != 0)
                    g.drawText(noteText, noteRect.translated(dx, dy), juce::Justification::centred, false);
        g.setColour(Palette::textBright);
        g.drawText(noteText, noteRect, juce::Justification::centred, false);

        const juce::String tgt = HumHouseVocalTuneProcessor::midiNoteToName(displayTargetMidi);
        g.setColour(Palette::textDim);
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.drawText("Target: " + (tgt.isEmpty() ? juce::String("--") : tgt),
                   static_cast<int>(cx) - 50, static_cast<int>(cy) + 84, 100, 16,
                   juce::Justification::centred, false);
    }

    // Piano keys: a dot shows the note being sung, a ring shows the note it is being tuned to
    auto marker = [&](int midi, bool isTarget)
    {
        if (midi < 0)
            return;

        const auto key = noteButtons[static_cast<size_t>(((midi % 12) + 12) % 12)]
                             .getBounds().toFloat() * (1.0f / s);
        const float mx = key.getCentreX();
        const float my = key.getBottom() - 13.0f;

        if (isTarget)
        {
            g.setColour(Palette::accentBright);
            g.drawEllipse(mx - 8.0f, my - 8.0f, 16.0f, 16.0f, 2.0f);
        }
        else
        {
            g.setColour(Palette::heatCold.withAlpha(0.35f));
            g.fillEllipse(mx - 8.0f, my - 8.0f, 16.0f, 16.0f);
            g.setColour(Palette::heatCold);
            g.fillEllipse(mx - 4.0f, my - 4.0f, 8.0f, 8.0f);
        }
    };
    marker(displayTargetMidi, true);
    marker(displayDetectedMidi, false);
}

// ---------------------------------------------------------------------------
// Layout (design coordinates, scaled)
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::resized()
{
    const float s = scaleFactor();
    lnf.setScale(s);

    auto R = [s](int x, int y, int w, int h)
    {
        return juce::Rectangle<int>(juce::roundToInt(static_cast<float>(x) * s),
                                    juce::roundToInt(static_cast<float>(y) * s),
                                    juce::roundToInt(static_cast<float>(w) * s),
                                    juce::roundToInt(static_cast<float>(h) * s));
    };

    auto place = [&](juce::Slider& k, juce::Label& l, int x, int y, int size)
    {
        k.setBounds(R(x, y, size, size));
        l.setBounds(R(x - 8, y + size - 2, size + 16, 16));
    };

    // Small caps labels never get smaller than 9 px
    const juce::Font labelFont(juce::FontOptions(juce::jmax(9.0f, 11.0f * s)));
    for (auto* l : { &speedLabel, &humanizeLabel, &inputGainLabel, &outputGainLabel,
                     &referenceFreqLabel, &rootNoteLabel })
        l->setFont(labelFont);

    // --- Top bar ---
    enableBtn.setBounds(R(kDesignW - 112, 8, 98, 28));

    // --- Left column: input / output gain ---
    place(inputGainKnob,  inputGainLabel,  18, 116, 52);
    place(outputGainKnob, outputGainLabel, 18, 194, 52);

    // --- Right panel: big Retune Speed knob, with Humanize underneath ---
    place(speedKnob,    speedLabel,    553,  78, 150);
    place(humanizeKnob, humanizeLabel, 586, 254,  84);

    // --- Key, scale buttons, switches ---
    const int rowY = 370;
    const int rowH = 28;
    rootNoteLabel.setBounds(R(14, rowY, 32, rowH));
    rootNoteBox.setBounds(R(50, rowY, 64, rowH));

    majorBtn.setBounds(R(126, rowY, 74, rowH));
    minorBtn.setBounds(R(206, rowY, 74, rowH));
    chromBtn.setBounds(R(286, rowY, 92, rowH));

    stabilizerBtn.setBounds(R(394, rowY, 134, rowH));
    formantBtn.setBounds(R(534, rowY, 92, rowH));
    rangeBox.setBounds(R(632, rowY, 134, rowH));

    // --- Pitch reference knob (monitor panel) ---
    place(referenceFreqKnob, referenceFreqLabel, 714, 540, 50);

    // --- Piano keys ---
    const int pianoX = 28;
    const int pianoY = 454;
    const int whiteW = 78;
    const int whiteH = 142;
    const int blackW = 48;
    const int blackH = 86;

    const int whiteIdx[] = { 0, 2, 4, 5, 7, 9, 11 };
    const int blackIdx[] = { 1, 3, 6, 8, 10 };

    for (int i = 0; i < 7; ++i)
        noteButtons[static_cast<size_t>(whiteIdx[i])]
            .setBounds(R(pianoX + i * (whiteW + 2), pianoY, whiteW, whiteH));

    // Black keys sit between the white ones
    const int blackPositions[] = { 0, 1, 3, 4, 5 };
    for (int i = 0; i < 5; ++i)
    {
        auto& btn = noteButtons[static_cast<size_t>(blackIdx[i])];
        const int xOff = pianoX + blackPositions[i] * (whiteW + 2) + whiteW - blackW / 2 + 1;
        btn.setBounds(R(xOff, pianoY, blackW, blackH));
        btn.toFront(false);
    }

    // --- Artwork ---
    rebuildScaledArt();

    if (skullView.solid.isValid())
    {
        const int w = skullView.solid.getWidth();
        const int h = skullView.solid.getHeight();
        skullView.setBounds(juce::roundToInt(kCx * s) - w / 2, juce::roundToInt(kCy * s) - h / 2, w, h);
    }

    if (armLeftView.image.isValid())
    {
        const int w = armLeftView.image.getWidth();
        const int h = armLeftView.image.getHeight();
        const int top = juce::roundToInt(62.0f * s);
        armLeftView.setBounds(juce::roundToInt((kCx - 156.0f) * s) - w / 2, top, w, h);
        armRightView.setBounds(juce::roundToInt((kCx + 156.0f) * s) - w / 2, top, w, h);
        armLeftView.opacity  = 0.9f;
        armRightView.opacity = 0.9f;
    }
}
