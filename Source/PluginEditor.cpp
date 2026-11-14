#include "PluginEditor.h"
#include <cmath>

namespace {
const juce::StringArray kNoteNames{"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};

// Which piano keys are "black"
constexpr bool isBlackKey(int idx) {
  return idx == 1 || idx == 3 || idx == 6 || idx == 8 || idx == 10;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
HumHouseVocalTuneEditor::HumHouseVocalTuneEditor(HumHouseVocalTuneProcessor &p)
    : AudioProcessorEditor(p), proc(p) {
  setLookAndFeel(&lnf);
  enableBtn.getProperties().set("powerButton", true);

  addAndMakeVisible(orb);
  auto &apvts = proc.getAPVTS();
  // --- Knobs ---
  setupKnob(speedKnob, "RETUNE SPEED",
            "How fast the pitch snaps to the note. Turn right for faster.\n0 = "
            "instant (hard tune), 100 = "
            "slow and natural.");
  speedKnob.getProperties().set("displayScale", 100.0); // shown as 0..100
  speedKnob.getProperties().set(
      "displayFlip",
      true); // fully right = 0 (fastest), fully left = 100 (slowest)

  setupKnob(humanizeKnob, "HUMANIZE",
            "Eases off correction on sustained notes.");
  humanizeKnob.getProperties().set("displayScale", 100.0);
  humanizeKnob.setTextValueSuffix("%");
  setupKnob(referenceFreqKnob, "PITCH REF",
            "What frequency the note A4 is tuned to. 440 Hz is the standard.");
  referenceFreqKnob.setTextValueSuffix(" Hz");
  referenceFreqKnob.setSliderStyle(juce::Slider::IncDecButtons);
  referenceFreqKnob.setIncDecButtonsMode(
      juce::Slider::incDecButtonsNotDraggable);
  referenceFreqKnob.setNumDecimalPlacesToDisplay(1);
  referenceFreqKnob.setDoubleClickReturnValue(true, 440.0);
  referenceFreqKnob.setColour(juce::Slider::textBoxBackgroundColourId,
                              lnf.palette.bgDark);
  referenceFreqKnob.setColour(juce::Slider::textBoxTextColourId,
                              lnf.palette.textValue);
  referenceFreqKnob.setColour(juce::Slider::textBoxOutlineColourId,
                              lnf.palette.accentDeep);
  referenceFreqLabel.setText("A4 REFERENCE", juce::dontSendNotification);
  referenceFreqLabel.setJustificationType(juce::Justification::centredLeft);
  referenceFreqLabel.setColour(juce::Label::textColourId, lnf.palette.textDim);
  addAndMakeVisible(referenceFreqLabel);

  speedAtt = std::make_unique<SliderAttach>(apvts, "retune", speedKnob);
  humanizeAtt = std::make_unique<SliderAttach>(apvts, "humanize", humanizeKnob);
  referenceFreqAtt =
      std::make_unique<SliderAttach>(apvts, "referenceFreq", referenceFreqKnob);

  // --- Scale selector: western modes plus optional quarter-tone / Maqam scales
  // ---
  for (int i = 0; i < humtune::ScaleQuantizer::kNumScales; ++i)
    scaleBox.addItem(humtune::ScaleQuantizer::name(i), i + 1);
  scaleBox.setSelectedId(currentScaleType() + 1, juce::dontSendNotification);
  scaleBox.setTooltip("Choose the target-note scale. 24-TET and several Maqam "
                      "modes use quarter-tone targets.\n"
                      "The piano keys show the nearest semitone classes; "
                      "manual key edits switch to a custom 12-TET "
                      "scale.");
  scaleBox.onChange = [this]() {
    const int scaleIndex = scaleBox.getSelectedId() - 1;
    if (scaleIndex < 0 || scaleIndex >= humtune::ScaleQuantizer::kNumScales)
      return;
    auto &state = proc.getAPVTS();
    if (scaleIndex < 3) {
      if (auto *legacyParam = state.getParameter("scaleType"))
        legacyParam->setValueNotifyingHost(
            legacyParam->convertTo0to1(static_cast<float>(scaleIndex)));
      if (auto *extensionParam = state.getParameter("scaleExtension"))
        extensionParam->setValueNotifyingHost(
            extensionParam->convertTo0to1(0.0f));
    } else {
      // Extension 1 maps to full scale index 3, extension 17 to index 19.
      if (auto *extensionParam = state.getParameter("scaleExtension"))
        extensionParam->setValueNotifyingHost(
            extensionParam->convertTo0to1(static_cast<float>(scaleIndex - 2)));
    }
    proc.applyScaleToKeys(scaleIndex, currentRoot());
    lastAppliedScale = scaleIndex;
    lastAppliedRoot = currentRoot();
    repaint();
  };
  addAndMakeVisible(scaleBox);

  enableBtn.setTooltip("Turn pitch correction on or off.");
  addAndMakeVisible(enableBtn);
  enableAtt = std::make_unique<ButtonAttach>(apvts, "enabled", enableBtn);

  // --- Voice range: the lowest note the tuner follows. Lower notes need more
  // delay. ---
  rangeBox.addItem("Soprano", 5);
  rangeBox.addItem("Alto / Tenor", 3);
  rangeBox.addItem("Low Male", 2);
  rangeBox.setTooltip(
      "Soprano: 180 Hz floor; Alto/Tenor: 100 Hz; Low Male: 80 Hz. A new "
      "selection uses "
      "its own range.");
  addAndMakeVisible(rangeBox);
  refreshVoiceType(
      static_cast<int>(proc.getAPVTS().getRawParameterValue("range")->load()));
  rangeBox.onChange = [this] {
    const int choice = rangeBox.getSelectedId() - 1;
    if (choice != 1 && choice != 2 && choice != 4)
      return;
    auto *parameter = proc.getAPVTS().getParameter("range");
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(
        parameter->convertTo0to1(static_cast<float>(choice)));
    parameter->endChangeGesture();
    // An explicit new voice selection must use that voice's actual floor.
    // Existing sessions retain their stored low-note protection until edited.
    if (auto *protection = proc.getAPVTS().getParameter("rangeProtection")) {
      protection->beginChangeGesture();
      protection->setValueNotifyingHost(0.0f);
      protection->endChangeGesture();
    }
  };

  // --- Key (root note) ---
  rootNoteBox.addItemList(kNoteNames, 1);
  rootNoteBox.setSelectedItemIndex(0, juce::dontSendNotification);
  rootNoteBox.setTooltip("The key of your song.\nChanging it re-lights the "
                         "piano keys for the chosen scale.");
  addAndMakeVisible(rootNoteBox);
  rootNoteLabel.setText("KEY", juce::dontSendNotification);
  rootNoteLabel.setJustificationType(juce::Justification::centredRight);
  rootNoteLabel.setColour(juce::Label::textColourId, lnf.palette.textDim);
  addAndMakeVisible(rootNoteLabel);
  rootNoteAtt = std::make_unique<ComboAttach>(apvts, "rootNote", rootNoteBox);

  // Host automation and GUI edits keep the piano projection in sync with the
  // scale/key.
  lastAppliedScale = currentScaleType();
  lastAppliedRoot = currentRoot();

  // --- Piano keys: lit = the tuner may snap to this note ---
  for (int i = 0; i < 12; ++i) {
    auto &btn = noteButtons[static_cast<size_t>(i)];
    btn.setButtonText(kNoteNames[i]);
    btn.setClickingTogglesState(true);
    btn.setToggleState(true, juce::dontSendNotification);
    btn.setTooltip("Click to allow or block this note.\nLit keys are the notes "
                   "the tuner can snap to.");
    // Tell the look-and-feel to draw this as a piano key (1 = white, 2 = black)
    btn.getProperties().set("pianoKey", isBlackKey(i) ? 2 : 1);
    addAndMakeVisible(btn);

    noteAtts[static_cast<size_t>(i)] =
        std::make_unique<ButtonAttach>(apvts, "note" + juce::String(i), btn);
  }

  heatmapDetected.fill(0.0f);
  heatmapTarget.fill(0.0f);

  // Readable at the design size, with proportional resizing.
  setResizable(true, true);
  setResizeLimits(630, 612, 1050, 1020);
  if (auto *c = getConstrainer())
    c->setFixedAspectRatio(static_cast<double>(kDesignW) /
                           static_cast<double>(kDesignH));
  setSize(juce::roundToInt(static_cast<float>(kDesignW)),
          juce::roundToInt(static_cast<float>(kDesignH)));

  appearanceButton.setName("Cycle theme");
  appearanceButton.setTooltip("Click the skull to cycle themes.");
  appearanceButton.getProperties().set("themeSkull", true);
  addAndMakeVisible(appearanceButton);
  appearanceButton.onClick = [this] {
    proc.setUiTheme((proc.getUiTheme() + 1) % 4);
    proc.updateHostDisplay(
        juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(
            true));
    applyAppearance();
  };
  settingsButton.setName("Settings");
  settingsButton.setTooltip("Formant, Low Latency and motion settings.");
  settingsButton.getProperties().set("settingsGear", true);
  addAndMakeVisible(settingsButton);
  settingsButton.onClick = [this] {
    juce::PopupMenu menu;
    menu.setLookAndFeel(&lnf);
    const bool preserve =
        proc.getAPVTS().getRawParameterValue("formantPreserve")->load() > .5f;
    menu.addSectionHeader("Processing");
    menu.addItem(11, "Formant", true, preserve);
    menu.addSeparator();
    menu.addItem(
        1, "Low Latency", true,
        proc.getAPVTS().getRawParameterValue("lowLatencyMode")->load() > .5f);
    menu.addItem(3, "Reduce motion and lighting", true,
                 proc.getReducedMotion());
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&settingsButton),
        [safe = juce::Component::SafePointer<HumHouseVocalTuneEditor>(this)](
            int result) {
          if (safe == nullptr || result == 0)
            return;
          if (result == 11) {
            auto *parameter =
                safe->proc.getAPVTS().getParameter("formantPreserve");
            parameter->beginChangeGesture();
            const bool current = safe->proc.getAPVTS()
                                     .getRawParameterValue("formantPreserve")
                                     ->load() > .5f;
            parameter->setValueNotifyingHost(current ? 0.0f : 1.0f);
            parameter->endChangeGesture();
          } else if (result == 1) {
            auto *parameter =
                safe->proc.getAPVTS().getParameter("lowLatencyMode");
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(
                parameter->getValue() > .5f ? 0.0f : 1.0f);
            parameter->endChangeGesture();
          } else if (result == 3) {
            safe->proc.setReducedMotion(!safe->proc.getReducedMotion());
            safe->proc.updateHostDisplay(
                juce::AudioProcessor::ChangeDetails{}
                    .withNonParameterStateChanged(true));
          }
        });
  };
  applyAppearance();
  startTimerHz(30);
}

HumHouseVocalTuneEditor::~HumHouseVocalTuneEditor() {
  juce::PopupMenu::dismissAllActiveMenus();
  setLookAndFeel(nullptr);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::setupKnob(juce::Slider &knob,
                                        const juce::String &text,
                                        const juce::String &tooltip) {
  knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
  knob.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
  knob.onDragStart = [&knob] {
    knob.getProperties().set("showDragValue", true);
    knob.repaint();
  };
  knob.onDragEnd = [&knob] {
    knob.getProperties().set("showDragValue", false);
    knob.repaint();
  };
  knob.setName(text);
  knob.setTooltip(tooltip);
  addAndMakeVisible(knob);
}

void HumHouseVocalTuneEditor::refreshVoiceType(int voice) {
  lastDisplayedVoice = voice;
  if (voice == 1 || voice == 2 || voice == 4)
    rangeBox.setSelectedId(voice + 1, juce::dontSendNotification);
  else
    rangeBox.setText("Legacy range " + juce::String(voice + 1),
                     juce::dontSendNotification);
}

int HumHouseVocalTuneEditor::currentScaleType() const {
  const int extension =
      static_cast<int>(*proc.getAPVTS().getRawParameterValue("scaleExtension"));
  return extension > 0 ? extension + 2
                       : static_cast<int>(*proc.getAPVTS().getRawParameterValue(
                             "scaleType"));
}

int HumHouseVocalTuneEditor::currentRoot() const {
  return static_cast<int>(*proc.getAPVTS().getRawParameterValue("rootNote"));
}

void HumHouseVocalTuneEditor::applyScaleFromControls() {
  const int scale = currentScaleType();
  const int root = currentRoot();
  proc.applyScaleToKeys(scale, root);
  lastAppliedScale = scale;
  lastAppliedRoot = root;
  if (scaleBox.getSelectedId() != scale + 1)
    scaleBox.setSelectedId(scale + 1, juce::dontSendNotification);
  repaint();
}

// The message-thread timer observes root/scale automation. No editor listener
// posts message-queue work from a host's audio-thread parameter callback.

// ---------------------------------------------------------------------------
// Material appearance shared by the editor and child controls
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::applyAppearance() {
  appearanceTheme = proc.getUiTheme();
  lnf.setTheme(appearanceTheme);
  orb.accent = lnf.palette.accentBright;
  orb.metalHigh = lnf.palette.metalHigh;
  orb.metalLow = lnf.palette.metalLow;
  rootNoteLabel.setColour(juce::Label::textColourId, lnf.palette.bodyText);
  referenceFreqLabel.setColour(juce::Label::textColourId, lnf.palette.bodyText);
  referenceFreqKnob.setColour(juce::Slider::textBoxTextColourId,
                              lnf.palette.textValue);
  referenceFreqKnob.setColour(juce::Slider::textBoxOutlineColourId,
                              lnf.palette.edge);
  sendLookAndFeelChange();
  repaint();
}

void HumHouseVocalTuneEditor::timerCallback() {
  displayDetectedHz = proc.getDetectedHz();
  displayTargetHz = proc.getTargetHz();
  displayConfidence = proc.getConfidence();
  displayDetectedMidi = proc.getDetectedMidiNote();
  displayTargetMidi = proc.getTargetMidiNote();
  displayInputLevel = proc.getInputLevel();
  const bool powered = enableBtn.getToggleState();
  for (auto *control : std::array<juce::Component *, 8>{
           &speedKnob, &humanizeKnob, &referenceFreqKnob, &rangeBox,
           &rootNoteBox, &scaleBox, &appearanceButton, &settingsButton})
    control->setEnabled(powered);
  for (auto &key : noteButtons)
    key.setEnabled(powered);
  if (powered && !proc.getReducedMotion()) {
    if (--lightingHold <= 0) {
      lightingTarget = .4f + .6f * lightingRandom.nextFloat();
      lightingHold = 45 + lightingRandom.nextInt(110);
    }
    if (--flareHold <= 0) {
      lightingFlare = .65f + .35f * lightingRandom.nextFloat();
      flareHold = 180 + lightingRandom.nextInt(180);
    }
    lightingLevel += .035f * (lightingTarget - lightingLevel);
    lightingFlare *= .86f;
    lightingPhase += .025;
  } else {
    lightingFlare = 0;
    lightingLevel = powered ? .6f : 0;
  }
  lnf.illumination = lightingLevel;
  lnf.flare = lightingFlare;
  lnf.lightPhase = static_cast<float>(lightingPhase);
  const int voice =
      static_cast<int>(proc.getAPVTS().getRawParameterValue("range")->load());
  // A ComboBox change callback is asynchronous. Refresh only on a real
  // parameter change, so a timer tick cannot replace a pending user selection.
  if (voice != lastDisplayedVoice)
    refreshVoiceType(voice);
  if (!powered) {
    displayDetectedHz = displayTargetHz = displayInputLevel =
        displayConfidence = 0;
    displayDetectedMidi = displayTargetMidi = -1;
    heatmapDetected.fill(0);
    heatmapTarget.fill(0);
  }
  // Pitch heatmap
  heatmapDetected[static_cast<size_t>(heatmapWriteIdx)] = displayDetectedHz;
  heatmapTarget[static_cast<size_t>(heatmapWriteIdx)] = displayTargetHz;
  heatmapWriteIdx = (heatmapWriteIdx + 1) % kHeatmapWidth;

  // Reflect host automation and presets. Only rewrite the keyboard when the
  // named scale or root actually changes, so a user's manual key mask is not
  // overwritten.
  const int scaleVal = currentScaleType();
  const int root = currentRoot();
  if (scaleVal != lastAppliedScale || root != lastAppliedRoot) {
    if (scaleVal != lastAppliedScale)
      scaleBox.setSelectedId(scaleVal + 1, juce::dontSendNotification);
    proc.applyScaleToKeys(scaleVal, root);
    lastAppliedScale = scaleVal;
    lastAppliedRoot = root;
  }

  if (appearanceTheme != proc.getUiTheme())
    applyAppearance();
  orb.bypassed = !enableBtn.getToggleState();
  const float active = !orb.bypassed && displayConfidence > .3f
                           ? juce::jlimit(0.0f, 1.0f, displayInputLevel * 2.0f)
                           : 0.0f;
  orb.activity += .2f * (active - orb.activity);
  orb.reducedMotion = proc.getReducedMotion();
  if (!orb.reducedMotion && active > .01f) {
    orb.motionLevel += .2f * (active - orb.motionLevel);
    orb.phase += .16 * active;
  }
  for (int i = 0; i < 12; ++i)
    noteButtons[static_cast<size_t>(i)].getProperties().set(
        "target", i == displayTargetMidi % 12 && displayTargetMidi >= 0);
  orb.intensity = static_cast<float>(speedKnob.getValue()) *
                  (1.0f - .65f * static_cast<float>(humanizeKnob.getValue()));
  if (!orb.reducedMotion && !orb.bypassed)
    orb.globePhase += .012 + .025 * active;
  orb.repaint();

  repaint();
}

// ---------------------------------------------------------------------------
// Paint (everything is drawn in design coordinates and scaled)
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::paint(juce::Graphics &g) {
  const float scale = scaleFactor();
  const auto &p = lnf.palette;
  juce::ColourGradient body(p.surface, 0, 0, p.body, 0,
                            static_cast<float>(getHeight()), false);
  g.setGradientFill(body);
  g.fillAll();
  g.addTransform(juce::AffineTransform::scale(scale));
  auto text = [&](juce::String value, int x, int y, int w, int h, float size,
                  juce::Colour colour,
                  juce::Justification align = juce::Justification::centred) {
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(size).withStyle("Bold")));
    g.drawText(value, x, y, w, h, align, false);
  };
  auto panel = [&](juce::Rectangle<float> bounds, juce::Colour fill,
                   float radius) {
    g.setColour(juce::Colours::black.withAlpha(.12f));
    g.fillRoundedRectangle(bounds.translated(0, 2), radius);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(p.edge);
    g.drawRoundedRectangle(bounds, radius, 1);
  };
  // One shared geometry serves all four materials.
  g.setColour(p.edge);
  g.drawRoundedRectangle(1, 1, kDesignW - 2, kDesignH - 2, 12, 1);
  for (auto point :
       {juce::Point<float>(13, 13), juce::Point<float>(687, 13),
        juce::Point<float>(13, 667), juce::Point<float>(687, 667)}) {
    juce::ColourGradient screw(p.metalHigh, point.x - 5, point.y - 5,
                               p.metalLow, point.x + 5, point.y + 5, false);
    g.setGradientFill(screw);
    g.fillEllipse(point.x - 5, point.y - 5, 10, 10);
    g.setColour(p.bgDark);
    g.drawLine(point.x - 2, point.y + 2, point.x + 2, point.y - 2, 1);
  }
  panel({24, 16, 198, 48}, p.bgDark, 8);
  text("SKELLY", 72, 23, 88, 26, 20, p.textBright,
       juce::Justification::centredLeft);
  text("TUNE", 149, 23, 72, 26, 20, juce::Colour(0xffacfa78),
       juce::Justification::centredLeft);
  text("PITCH CORRECTION", 73, 49, 156, 11, 8, p.textDim,
       juce::Justification::centredLeft);

  panel({24, 84, 652, 286}, p.surface, 12);
  text("RETUNE", 36, 103, 160, 22, 16, p.bodyText);
  text("HUMANIZE", 504, 103, 160, 22, 16, p.bodyText);
  text("SLOW", 38, 331, 54, 20, 11, p.bodyText,
       juce::Justification::centredLeft);
  text("FAST", 142, 331, 54, 20, 11, p.bodyText,
       juce::Justification::centredRight);
  text("LESS", 506, 331, 54, 20, 11, p.bodyText,
       juce::Justification::centredLeft);
  text("MORE", 610, 331, 54, 20, 11, p.bodyText,
       juce::Justification::centredRight);
  text("KEY", 386, 8, 52, 14, 9, p.bodyText, juce::Justification::centredLeft);
  text("SCALE", 450, 8, 154, 14, 9, p.bodyText,
       juce::Justification::centredLeft);
  text("INPUT TYPE", 234, 8, 140, 14, 9, p.bodyText,
       juce::Justification::centredLeft);
  panel({24, 380, 652, 64}, p.bgDark, 9);
  text("ORIGINAL", 270, 386, 80, 24, 10, p.heatCold);
  text("TARGET", 366, 386, 70, 24, 10, p.heatWarm);
  text("A4", 490, 386, 30, 24, 10, p.textDim);
  const auto pitchY = [](float hz) {
    const float midi = 69 + 12 * std::log2(juce::jmax(1.0f, hz) / 440.0f);
    return juce::jlimit(418.0f, 436.0f, 436 - (midi - 36) * 18 / 48);
  };
  for (int midi : {36, 84}) {
    const float y = 436 - (midi - 36) * 18 / 48.0f;
    g.setColour(p.accentDeep.withAlpha(.6f));
    g.drawLine(63, y, 660, y, .7f);
    text("C" + juce::String(midi / 12 - 1), 32, static_cast<int>(y) - 6, 28, 12,
         9, p.textDim);
  }
  juce::Path sung, target;
  bool sungOpen = false, targetOpen = false;
  for (int i = 0; i < kHeatmapWidth; ++i) {
    const auto index =
        static_cast<size_t>((heatmapWriteIdx + i) % kHeatmapWidth);
    const float x = 64 + i * 596.0f / (kHeatmapWidth - 1);
    const float original = heatmapDetected[index],
                corrected = heatmapTarget[index];
    if (original >= 35) {
      if (sungOpen)
        sung.lineTo(x, pitchY(original));
      else
        sung.startNewSubPath(x, pitchY(original));
      sungOpen = true;
    } else
      sungOpen = false;
    if (corrected >= 35) {
      if (targetOpen)
        target.lineTo(x, pitchY(corrected));
      else
        target.startNewSubPath(x, pitchY(corrected));
      targetOpen = true;
    } else
      targetOpen = false;
  }
  g.setColour(p.heatCold);
  g.strokePath(sung, juce::PathStrokeType(1.2f));
  g.setColour(p.heatWarm);
  g.strokePath(target, juce::PathStrokeType(1.8f));
  text("ALLOWED NOTES", 30, 460, 300, 18, 10, p.bodyText.withAlpha(.75f), juce::Justification::centredLeft);
  panel({24, 490, 652, 170}, p.bgDark, 8);
}

void HumHouseVocalTuneEditor::paintOverChildren(juce::Graphics &g) {
  g.addTransform(juce::AffineTransform::scale(scaleFactor()));
  if (displayDetectedMidi >= 0 && enableBtn.getToggleState()) {
    const auto bounds =
        noteButtons[static_cast<size_t>(displayDetectedMidi % 12)]
            .getBounds()
            .toFloat() /
        scaleFactor();
    g.setColour(lnf.palette.heatCold);
    g.drawEllipse(bounds.getCentreX() - 4, bounds.getBottom() - 41, 8, 8, 1.8f);
  }
  if (!enableBtn.getToggleState()) {
    g.excludeClipRegion(
        (enableBtn.getBounds().toFloat() / scaleFactor()).toNearestInt());
    g.setColour(juce::Colours::black.withAlpha(.76f));
    g.fillAll();
    g.setColour(juce::Colour(0xff778079));
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("POWER OFF", 24, 380, 652, 64, juce::Justification::centred);
  }
}

void HumHouseVocalTuneEditor::resized() {
  const float scale = scaleFactor();
  lnf.setScale(scale);
  auto rect = [scale](int x, int y, int w, int h) {
    return juce::Rectangle<int>(
        juce::roundToInt(x * scale), juce::roundToInt(y * scale),
        juce::roundToInt(w * scale), juce::roundToInt(h * scale));
  };
  speedKnob.setBounds(rect(40, 154, 152, 152));
  humanizeKnob.setBounds(rect(508, 154, 152, 152));
  orb.setBounds(rect(215, 95, 270, 270));
  rootNoteBox.setBounds(rect(386, 26, 52, 30));
  scaleBox.setBounds(rect(450, 26, 154, 30));
  referenceFreqKnob.setBounds(rect(526, 386, 138, 24));
  referenceFreqKnob.setTextBoxStyle(juce::Slider::TextBoxLeft, false,
                                    juce::roundToInt(90 * scale),
                                    juce::roundToInt(24 * scale));
  referenceFreqLabel.setVisible(false);
  rootNoteLabel.setVisible(false);
  appearanceButton.setBounds(rect(32, 24, 32, 34));
  settingsButton.setBounds(rect(624, 26, 28, 28));
  enableBtn.setBounds(rect(660, 26, 28, 28));
  rangeBox.setBounds(rect(234, 26, 140, 30));
  const int natural[] = {0, 2, 4, 5, 7, 9, 11}, sharps[] = {1, 3, 6, 8, 10},
            positions[] = {0, 1, 3, 4, 5};
  const auto edge = [&](int i) {
    return juce::roundToInt((29 + i * 642.0f / 7) * scale);
  };
  for (int i = 0; i < 7; ++i)
    noteButtons[static_cast<size_t>(natural[i])].setBounds(
        edge(i), juce::roundToInt(497 * scale), edge(i + 1) - edge(i) - 1,
        juce::roundToInt(156 * scale));
  for (int i = 0; i < 5; ++i) {
    auto &key = noteButtons[static_cast<size_t>(sharps[i])];
    key.setBounds(edge(positions[i] + 1) - juce::roundToInt(24 * scale),
                  juce::roundToInt(497 * scale), juce::roundToInt(48 * scale),
                  juce::roundToInt(100 * scale));
    key.toFront(false);
  }
}
