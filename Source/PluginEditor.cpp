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
constexpr float kCx = 390.0f;
constexpr float kCy = 206.0f;
constexpr float kR  = 116.0f;

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
              "Slows the correction response slightly to preserve natural movement.");
    humanizeKnob.getProperties().set("displayScale", 100.0);
    setupKnob(amountKnob, amountLabel, "AMOUNT",
              "How much of the detected pitch error is corrected. 100% fully corrects to the target note.");
    amountKnob.getProperties().set("displayScale", 100.0);
    setupKnob(referenceFreqKnob, referenceFreqLabel, "PITCH REF",
              "What frequency the note A4 is tuned to. 440 Hz is the standard.");
    referenceFreqKnob.setTextValueSuffix(" Hz");
    referenceFreqKnob.getProperties().set("hideValue", true);   // the value is shown next to the knob

    speedAtt     = std::make_unique<SliderAttach>(apvts, "retune",    speedKnob);
    humanizeAtt  = std::make_unique<SliderAttach>(apvts, "humanize", humanizeKnob);
    amountAtt    = std::make_unique<SliderAttach>(apvts, "amount", amountKnob);
    referenceFreqAtt = std::make_unique<SliderAttach>(apvts, "referenceFreq", referenceFreqKnob);

    // --- Scale selector. Choosing a scale also updates the piano-key mask. ---
    scaleBox.addItemList(juce::StringArray { "Major", "Minor", "Chromatic", "Harmonic Minor",
                                             "Major Pentatonic", "Minor Pentatonic", "Blues" }, 1);
    scaleBox.setSelectedItemIndex(currentScaleType(), juce::dontSendNotification);
    scaleBox.setTooltip("Choose the allowed notes in the selected key. The piano keys update to match.");
    addAndMakeVisible(scaleBox);
    scaleAtt = std::make_unique<ComboAttach>(apvts, "scaleType", scaleBox);
    scaleBox.onChange = [this]()
    {
        const int selectedScale = scaleBox.getSelectedItemIndex();
        if (selectedScale >= 0)
        {
            if (auto* param = proc.getAPVTS().getParameter("scaleType"))
                param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(selectedScale)));
            proc.applyScaleToKeys(selectedScale, currentRoot());
            repaint();
        }
    };

    // --- Toggle buttons ---
    stabilizerBtn.setTooltip("Holds a note a little longer, so vibrato\ndoes not make the tuning jump between notes.");
    formantBtn.setTooltip("On: keeps the natural character of the voice.\nOff: robotic / chipmunk effect.");
    enableBtn.setTooltip("Turns the tuning on and off.");
    lowLatencyBtn.setTooltip("On: shorter TD-PSOLA overlap and lower algorithmic delay. Off: longer overlap for a smoother, more forgiving sound. Input Type still defines the lowest note range.");

    addAndMakeVisible(stabilizerBtn);
    addAndMakeVisible(formantBtn);
    addAndMakeVisible(enableBtn);
    addAndMakeVisible(lowLatencyBtn);

    stabilizerAtt = std::make_unique<ButtonAttach>(apvts, "noteStabilizer",  stabilizerBtn);
    formantAtt    = std::make_unique<ButtonAttach>(apvts, "formantPreserve", formantBtn);
    enableAtt     = std::make_unique<ButtonAttach>(apvts, "enabled",         enableBtn);
    lowLatencyAtt = std::make_unique<ButtonAttach>(apvts, "lowLatencyEnabled", lowLatencyBtn);

    // --- Input Type replaces the old latency-mode selector. ---
    inputTypeBox.addItemList(juce::StringArray { "Soprano", "Alto / Tenor", "Low Male", "Bass", "Instrument" }, 1);
    inputTypeBox.setSelectedItemIndex(1, juce::dontSendNotification);
    inputTypeBox.setTooltip("Choose the source type so pitch detection starts with an appropriate expected range.");
    addAndMakeVisible(inputTypeBox);
    inputTypeAtt = std::make_unique<ComboAttach>(apvts, "inputType", inputTypeBox);

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
    setSize(683, 540);

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
        const int armH = juce::jmax(1, juce::roundToInt(250.0f * s));
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

    // Pitch heatmap
    heatmapDetected[static_cast<size_t>(heatmapWriteIdx)] = displayDetectedHz;
    heatmapTarget[static_cast<size_t>(heatmapWriteIdx)]   = displayTargetHz;
    heatmapWriteIdx = (heatmapWriteIdx + 1) % kHeatmapWidth;

    // Keep a faint idle aura, then let vocal energy and pitch confidence drive a
    // quick, controlled bloom. The central orb therefore remains readable in silence
    // but clearly responds to active, confidently tracked notes.
    const float ambient = 0.07f;
    const float glowTarget = juce::jlimit(0.0f, 1.0f,
        ambient + displayInputLevel * 1.85f + displayConfidence * 0.18f);
    const float smoothing = glowTarget > skullView.glow ? 0.42f : 0.16f;
    skullView.glow += smoothing * (glowTarget - skullView.glow);
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
        auto titleArea = juce::Rectangle<int>(48, 0, kDesignW - 96, 44);
        const juce::String title = "SkellyTune";
        g.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));

        g.setColour(Palette::accent.withAlpha(0.16f));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx != 0 || dy != 0)
                    g.drawText(title, titleArea.translated(dx, dy), juce::Justification::centred, false);

        g.setColour(Palette::accentBright);
        g.drawText(title, titleArea, juce::Justification::centred, false);

        // Version is intentionally omitted from the crowded top bar; the two
        // functional mode switches now have clear, non-overlapping hit areas.
    }

    // --- Main panel: controls flank the centered skeleton/radar ---
    panel(8.0f, 52.0f, 764.0f, 310.0f, 8.0f, Palette::bgPanel.withAlpha(0.9f), Palette::accentDim);
    panel(18.0f, 66.0f, 132.0f, 250.0f, 10.0f, Palette::bgSection.withAlpha(0.62f), Palette::accentDeep);
    panel(630.0f, 66.0f, 132.0f, 250.0f, 10.0f, Palette::bgSection.withAlpha(0.62f), Palette::accentDeep);

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

        const float pulse = 0.94f + 0.06f * std::sin(
            static_cast<float>(juce::Time::getMillisecondCounter() % 2600u) / 2600.0f * twoPi);
        const float liveGlow = juce::jlimit(0.0f, 1.0f, skullView.glow);
        juce::ColourGradient glow(Palette::accentBright.withAlpha(0.07f + 0.10f * liveGlow), cx, cy,
                                   Palette::accent.withAlpha(0.0f), cx, cy - R - 26.0f, true);
        g.setGradientFill(glow);
        g.fillEllipse(cx - R - 26.0f, cy - R - 26.0f, (R + 26.0f) * 2.0f, (R + 26.0f) * 2.0f);

        // The orb has its own stacked halo, rim and inner lens rather than relying on
        // the flat radar fill. Small intensity changes make it feel alive, not flashing.
        const float orbR = (R - 50.0f) * pulse;
        juce::ColourGradient orbHalo(Palette::accentBright.withAlpha(0.22f + 0.20f * liveGlow), cx, cy,
                                     Palette::accent.withAlpha(0.0f), cx + orbR, cy, true);
        g.setGradientFill(orbHalo);
        g.fillEllipse(cx - orbR - 10.0f, cy - orbR - 10.0f,
                      2.0f * (orbR + 10.0f), 2.0f * (orbR + 10.0f));

        g.setColour(Palette::accent.withAlpha(0.10f + 0.22f * liveGlow));
        g.drawEllipse(cx - orbR - 5.0f, cy - orbR - 5.0f,
                      2.0f * (orbR + 5.0f), 2.0f * (orbR + 5.0f), 3.0f);
        g.setColour(Palette::accent.withAlpha(0.32f + 0.36f * liveGlow));
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
            const float ro = R - 57.0f;
            juce::ColourGradient orbGrad(Palette::accentBright.withAlpha(0.18f + 0.16f * liveGlow), cx, cy,
                                          Palette::bgDark.withAlpha(0.96f), cx, cy - ro, true);
            g.setGradientFill(orbGrad);
            g.fillEllipse(cx - ro, cy - ro, ro * 2.0f, ro * 2.0f);
            g.setColour(Palette::accent.withAlpha(0.30f + 0.34f * liveGlow));
            g.drawEllipse(cx - ro, cy - ro, ro * 2.0f, ro * 2.0f, 1.5f);
            g.setColour(Palette::accentBright.withAlpha(0.14f + 0.16f * liveGlow));
            g.drawEllipse(cx - ro + 7.0f, cy - ro + 7.0f,
                          (ro - 7.0f) * 2.0f, (ro - 7.0f) * 2.0f, 1.0f);
        }
    }

    // --- Pitch history strip ---
    const juce::Rectangle<int> heatArea(20, 406, 740, 34);
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

    // --- Central monitor strip below the circular tuner ---
    panel(158.0f, 323.0f, 464.0f, 36.0f, 6.0f, Palette::bgSection, Palette::accentDim);

    const juce::String detNote = HumHouseVocalTuneProcessor::midiNoteToName(displayDetectedMidi);
    const juce::String tgtNote = HumHouseVocalTuneProcessor::midiNoteToName(displayTargetMidi);
    const double latencyMs = 1000.0 * static_cast<double>(proc.getLatencySamples())
                           / juce::jmax(1.0, proc.getSampleRate());
    const juce::String detValue = detNote.isEmpty() ? juce::String("--")
        : detNote + " " + juce::String(juce::roundToInt(displayDetectedHz)) + "Hz";
    const juce::String targetValue = tgtNote.isEmpty() ? juce::String("--")
        : tgtNote + " " + juce::String(juce::roundToInt(displayTargetHz)) + "Hz";
    const juce::String correctionValue = detNote.isEmpty() ? juce::String("--")
        : (displayCorrectionCents >= 0.0f ? "+" : "")
            + juce::String(juce::roundToInt(displayCorrectionCents)) + " cents";
    const juce::String latencyValue = juce::String(latencyMs, 1) + " ms";

    const juce::String labels[] = { "DETECTED", "TARGET", "CORRECTION", "LATENCY" };
    const juce::String values[] = { detValue, targetValue, correctionValue, latencyValue };
    for (int i = 0; i < 4; ++i)
    {
        const int x = 166 + i * 114;
        g.setFont(juce::Font(juce::FontOptions(9.5f).withStyle("Bold")));
        g.setColour(Palette::textDim);
        g.drawText(labels[i], x, 325, 108, 11, juce::Justification::centred, false);
        g.setFont(juce::Font(juce::FontOptions(11.5f).withStyle("Bold")));
        g.setColour(Palette::textValue);
        g.drawText(values[i], x, 337, 108, 17, juce::Justification::centred, false);
    }

    // Reference value sits beneath the small pitch-reference control in the right column.
    {
        const float refHz = *proc.getAPVTS().getRawParameterValue("referenceFreq");
        g.setFont(juce::Font(juce::FontOptions(10.5f).withStyle("Bold")));
        g.setColour(Palette::textValue);
        g.drawText("A4 " + juce::String(refHz, 1) + " Hz", 638, 301, 116, 13,
                   juce::Justification::centred, false);
    }

    // --- Full-width piano keyboard backing ---
    panel(20.0f, 446.0f, 740.0f, 158.0f, 6.0f, Palette::bgPanel, Palette::accentDim);

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
    for (auto* l : { &speedLabel, &humanizeLabel, &amountLabel, &referenceFreqLabel, &rootNoteLabel })
        l->setFont(labelFont);

    // --- Top bar ---
    // Switches have separate hit targets with consistent padding; the title remains clear.
    lowLatencyBtn.setBounds(R(kDesignW - 232, 8, 106, 28));
    enableBtn.setBounds(R(kDesignW - 118, 8, 104, 28));

    // --- Symmetrical side controls: Retune Speed left, Humanize right ---
    place(speedKnob,    speedLabel,     26, 112, 116);
    place(amountKnob,   amountLabel,    57, 242, 54);
    place(humanizeKnob, humanizeLabel, 638, 112, 116);

    // --- Key, scale buttons, switches ---
    const int rowY = 370;
    const int rowH = 28;
    rootNoteLabel.setBounds(R(14, rowY, 32, rowH));
    rootNoteBox.setBounds(R(50, rowY, 64, rowH));

    scaleBox.setBounds(R(126, rowY, 142, rowH));

    stabilizerBtn.setBounds(R(278, rowY, 134, rowH));
    formantBtn.setBounds(R(420, rowY, 92, rowH));
    inputTypeBox.setBounds(R(520, rowY, 246, rowH));

    // --- Pitch reference knob in the right-hand control column ---
    place(referenceFreqKnob, referenceFreqLabel, 684, 242, 44);

    // --- Piano keys ---
    const int pianoX = 28;
    const int pianoY = 454;
    const int whiteW = 100;
    const int whiteH = 142;
    const int blackW = 58;
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
        const int top = juce::roundToInt(68.0f * s);
        armLeftView.setBounds(juce::roundToInt((kCx - 156.0f) * s) - w / 2, top, w, h);
        armRightView.setBounds(juce::roundToInt((kCx + 156.0f) * s) - w / 2, top, w, h);
        armLeftView.opacity  = 0.9f;
        armRightView.opacity = 0.9f;
    }
}
