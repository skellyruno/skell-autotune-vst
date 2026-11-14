#include "PluginEditor.h"
#include <cmath>

namespace {
const juce::StringArray kNoteNames{"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};

// Which piano keys are "black"
constexpr bool isBlackKey(int idx) {
  return idx == 1 || idx == 3 || idx == 6 || idx == 8 || idx == 10;
}

class SettingsRow final : public juce::PopupMenu::CustomComponent {
public:
  SettingsRow(juce::String label, bool checked, std::function<void()> action,
              juce::Colour textColour, float scale)
      : CustomComponent(false), caption(std::move(label)), active(checked),
        colour(textColour), zoom(scale) {
    button.setTitle(caption);
    button.setAccessible(true);
    button.onClick = [this, action = std::move(action)] {
      action();
      active = !active;
      repaint();
    };
    addAndMakeVisible(button);
  }
  void getIdealSize(int& width, int& height) override {
    width = juce::roundToInt(158 * zoom);
    height = juce::roundToInt(29 * zoom);
  }
  void resized() override { button.setBounds(getLocalBounds()); }
  void paintOverChildren(juce::Graphics& g) override {
    auto area = getLocalBounds().reduced(juce::roundToInt(8 * zoom), 1);
    if (button.isOver() || button.hasKeyboardFocus(false)) {
      g.setColour(colour.withAlpha(.12f));
      g.fillRoundedRectangle(area.toFloat(), 4.0f);
    }
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(14 * zoom)));
    auto mark = area.removeFromRight(juce::roundToInt(18 * zoom)).toFloat();
    g.drawText(caption, area, juce::Justification::centredLeft, false);
    if (active) {
      const auto c = mark.getCentre();
      juce::Path check;
      check.startNewSubPath(c.x - 5 * zoom, c.y);
      check.lineTo(c.x - 1 * zoom, c.y + 4 * zoom);
      check.lineTo(c.x + 6 * zoom, c.y - 4 * zoom);
      g.strokePath(check, juce::PathStrokeType(1.8f * zoom));
    }
  }
private:
  struct RowButton final : juce::Button {
    RowButton() : Button("Setting") {}
    void paintButton(juce::Graphics&, bool, bool) override {}
  } button;
  juce::String caption;
  bool active;
  juce::Colour colour;
  float zoom;
};

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
HumHouseVocalTuneEditor::HumHouseVocalTuneEditor(HumHouseVocalTuneProcessor &p)
    : AudioProcessorEditor(p), proc(p) {
  setOpaque(true);
  setLookAndFeel(&lnf);
  enableBtn.getProperties().set("powerButton", true);

  addAndMakeVisible(orb);
  auto &apvts = proc.getAPVTS();
  // --- Knobs ---
  setupKnob(speedKnob, "RETUNE SPEED",
            "How fast the pitch snaps to the note. Turn right for faster.\n0 = "
            "instant (hard tune), 100 = "
            "slow and natural.");
  setupKnob(humanizeKnob, "HUMANIZE",
            "Eases off correction on sustained notes.");

  setupKnob(referenceFreqKnob, "PITCH REF",
            "What frequency the note A4 is tuned to. 440 Hz is the standard.");
  referenceFreqKnob.setTextValueSuffix(" Hz");
  referenceFreqKnob.setSliderStyle(juce::Slider::LinearHorizontal);
  referenceFreqKnob.getProperties().set("referenceReadout", true);
  referenceFreqKnob.setScrollWheelEnabled(true);
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
  for (auto* step : {&referenceDown, &referenceUp}) {
    const bool up = step == &referenceUp;
    step->setTitle(up ? "Raise A4 reference" : "Lower A4 reference");
    step->getProperties().set("referenceStep", true);
    step->onClick = [this, up] {
      const auto interval = referenceFreqKnob.getInterval();
      referenceFreqKnob.setValue(referenceFreqKnob.getValue() + (up ? interval : -interval),
                                 juce::sendNotificationSync);
    };
    addAndMakeVisible(*step);
  }
  for (auto* value : {&retuneValue, &humanizeValue}) {
    value->setJustificationType(juce::Justification::centred);
    value->setInterceptsMouseClicks(false, false);
    addAndMakeVisible(*value);
  }
  speedKnob.onValueChange = [this] {
    retuneValue.setText(juce::String(juce::roundToInt(100 * (1 - speedKnob.getValue()))),
                        juce::dontSendNotification);
  };
  humanizeKnob.onValueChange = [this] {
    humanizeValue.setText(juce::String(juce::roundToInt(100 * humanizeKnob.getValue())),
                          juce::dontSendNotification);
  };
  speedKnob.onValueChange();
  humanizeKnob.onValueChange();

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
  rootNoteBox.getProperties().set("keyReadout", true);
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
  setResizeLimits(501, 486, 1050, 1020);
  if (auto *c = getConstrainer())
    c->setFixedAspectRatio(static_cast<double>(kDesignW) /
                           static_cast<double>(kDesignH));
  setSize(501, 486);

  appearanceButton.setName("Cycle theme");
  appearanceButton.setTooltip("Click the skull to cycle themes.");
  appearanceButton.getProperties().set("themeSkull", true);
  addAndMakeVisible(appearanceButton);
  appearanceButton.onClick = [this] {
    proc.setUiTheme((proc.getUiTheme() + 1) % 4);
    themeNotificationPending = true;
    themeChangedAt = juce::Time::getMillisecondCounterHiRes();
    applyAppearance();
  };
  settingsButton.setName("Settings");
  settingsButton.setTooltip("Formant, Low Latency and motion settings.");
  settingsButton.getProperties().set("settingsGear", true);
  addAndMakeVisible(settingsButton);
  settingsButton.onClick = [this] {
    juce::PopupMenu menu;
    menu.setLookAndFeel(&lnf);
    const auto safe = juce::Component::SafePointer<HumHouseVocalTuneEditor>(this);
    const auto addParameter = [&](int id, const char* label, const char* parameterId) {
      const auto checked = proc.getAPVTS().getParameter(parameterId)->getValue() > .5f;
      menu.addCustomItem(id, std::make_unique<SettingsRow>(label, checked,
          [safe, parameterId] {
            if (safe == nullptr) return;
            auto* parameter = safe->proc.getAPVTS().getParameter(parameterId);
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->getValue() > .5f ? 0.f : 1.f);
            parameter->endChangeGesture();
          }, lnf.palette.bodyText, scaleFactor()));
    };
    addParameter(11, "Formant", "formantPreserve");
    addParameter(1, "Low Latency", "lowLatencyMode");
    menu.addCustomItem(3, std::make_unique<SettingsRow>("Reduce motion", proc.getReducedMotion(),
        [safe] {
          if (safe == nullptr) return;
          safe->proc.setReducedMotion(!safe->proc.getReducedMotion());
          safe->proc.updateHostDisplay(juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true));
        }, lnf.palette.bodyText, scaleFactor()));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&settingsButton)
                           .withParentComponent(this));
  };
  applyAppearance();
  lastAnimationTick = juce::Time::getMillisecondCounterHiRes();
  startTimerHz(30);
}

HumHouseVocalTuneEditor::~HumHouseVocalTuneEditor() {
  stopTimer();
  juce::PopupMenu::dismissAllActiveMenus();
  if (themeNotificationPending)
    proc.updateHostDisplay(juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true));
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
  knob.setMouseDragSensitivity(220);
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
  for (auto* value : {&retuneValue, &humanizeValue})
    value->setColour(juce::Label::textColourId, lnf.palette.bodyText);
  orb.accent = lnf.palette.accentBright;
  orb.metalHigh = lnf.palette.metalHigh;
  orb.metalLow = lnf.palette.metalLow;
  rootNoteLabel.setColour(juce::Label::textColourId, lnf.palette.bodyText);
  referenceFreqLabel.setColour(juce::Label::textColourId, lnf.palette.bodyText);
  referenceFreqKnob.setColour(juce::Slider::textBoxTextColourId,
                              lnf.palette.textValue);
  referenceFreqKnob.setColour(juce::Slider::textBoxOutlineColourId,
                              lnf.palette.edge);
  backdrop = {};
  repaint();
}

void HumHouseVocalTuneEditor::timerCallback() {
  if (!isShowing()) return;
  const auto now = juce::Time::getMillisecondCounterHiRes();
  const float elapsed = static_cast<float>(juce::jlimit(0.0, .08, (now - lastAnimationTick) * .001));
  lastAnimationTick = now;
  if (themeNotificationPending && now - themeChangedAt >= 180) {
    themeNotificationPending = false;
    proc.updateHostDisplay(juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true));
  }

  const int previousDetected = displayDetectedMidi;
  const int previousTarget = displayTargetMidi;
  const bool powered = enableBtn.getToggleState();
  const bool powerChanged = powered != lastPowered;
  if (powerChanged) {
    lastPowered = powered;
    for (auto *control : std::array<juce::Component *, 10>{
             &speedKnob, &humanizeKnob, &referenceFreqKnob, &rangeBox,
             &rootNoteBox, &scaleBox, &appearanceButton, &settingsButton,
             &referenceDown, &referenceUp})
      control->setEnabled(powered);
    for (auto &key : noteButtons)
      key.setEnabled(powered);
    repaint();
  }

  displayDetectedHz = powered ? proc.getDetectedHz() : 0;
  displayTargetHz = powered ? proc.getTargetHz() : 0;
  displayConfidence = powered ? proc.getConfidence() : 0;
  displayDetectedMidi = powered ? proc.getDetectedMidiNote() : -1;
  displayTargetMidi = powered ? proc.getTargetMidiNote() : -1;
  displayInputLevel = powered ? proc.getInputLevel() : 0;
  const int voice =
      static_cast<int>(proc.getAPVTS().getRawParameterValue("range")->load());
  // A ComboBox change callback is asynchronous. Refresh only on a real
  // parameter change, so a timer tick cannot replace a pending user selection.
  if (voice != lastDisplayedVoice)
    refreshVoiceType(voice);
  const auto slot = static_cast<size_t>(heatmapWriteIdx);
  if (displayDetectedHz > 0 || displayTargetHz > 0 || historyFrames > 0 || powerChanged
      || previousDetected != displayDetectedMidi || previousTarget != displayTargetMidi) {
    if (displayDetectedHz > 0 || displayTargetHz > 0)
      historyFrames = kHeatmapWidth;
    else if (historyFrames > 0)
      --historyFrames;
    if (!powered) {
      heatmapDetected.fill(0);
      heatmapTarget.fill(0);
      historyFrames = 0;
    }
    heatmapDetected[slot] = displayDetectedHz;
    heatmapTarget[slot] = displayTargetHz;
    heatmapWriteIdx = (heatmapWriteIdx + 1) % kHeatmapWidth;
    repaint(juce::Rectangle<float>(24, 386, 652, 58).transformedBy(juce::AffineTransform::scale(scaleFactor()))
                .getSmallestIntegerContainer());
  }

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
  const bool motion = powered && !proc.getReducedMotion();
  const bool motionChanged = orb.reducedMotion == motion;
  orb.bypassed = !powered;
  orb.reducedMotion = !motion;
  const float active = motion && displayInputLevel > .002f
                           ? std::sqrt(juce::jlimit(0.0f, 1.0f, displayInputLevel * 3.5f))
                           : 0.0f;
  const float oldActivity = orb.activity;
  orb.activity += (1.0f - std::exp(-elapsed / (active > oldActivity ? .025f : .12f)))
                  * (active - oldActivity);
  if (!motion || orb.activity < .003f)
    orb.activity = 0;
  orb.motionLevel = orb.activity * (.6f + .4f * juce::jlimit(0.0f, 1.0f, displayConfidence));
  orb.phase = std::fmod(orb.phase + .49 * elapsed * (3.0 + 20.0 * orb.activity),
                        juce::MathConstants<double>::twoPi * 10.0);
  orb.intensity = .75f + .25f * static_cast<float>(speedKnob.getValue());
  if (motion)
    orb.sweepPhase = std::fmod(orb.sweepPhase + .60 * elapsed * (.42 + 1.2 * speedKnob.getValue()
                                        + 1.75 * orb.activity),
                               juce::MathConstants<double>::twoPi);
  // Near-idle sweep stays visible at 15 Hz, while audio can animate at 30 Hz.
  if (powerChanged || motionChanged || (motion &&
      now - lastOrbRepaint >= (orb.activity > 0.015f ? 30.0 : 65.0))) {
    lastOrbRepaint = now;
    orb.repaint();
    repaint(juce::Rectangle<float>(148, 449, 511, 37)
                .transformedBy(juce::AffineTransform::scale(scaleFactor()))
                .getSmallestIntegerContainer());
  }

  const float illumination = .3f + .7f * orb.activity;
  if (std::abs(illumination - lnf.illumination) >= .025f || powerChanged || motionChanged) {
    lnf.illumination = illumination;
    lnf.flare = orb.activity;
    lnf.lightPhase = static_cast<float>(orb.phase);
    speedKnob.repaint();
    humanizeKnob.repaint();
  }

  for (int i = 0; i < 12; ++i) {
    auto &key = noteButtons[static_cast<size_t>(i)];
    const bool target = displayTargetMidi >= 0 && i == displayTargetMidi % 12;
    const bool oldTarget = previousTarget >= 0 && i == previousTarget % 12;
    const bool markerMoved = previousDetected != displayDetectedMidi
        && ((previousDetected >= 0 && i == previousDetected % 12)
            || (displayDetectedMidi >= 0 && i == displayDetectedMidi % 12));
    if (target != oldTarget)
      key.getProperties().set("target", target);
    if (target != oldTarget || markerMoved)
      key.repaint();
  }
}

// ---------------------------------------------------------------------------
// Paint (everything is drawn in design coordinates and scaled)
// ---------------------------------------------------------------------------
void HumHouseVocalTuneEditor::paintBackdrop(juce::Graphics &g) {
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
  text("RETUNE", 36, 124, 160, 22, 16, p.bodyText);
  text("HUMANIZE", 504, 124, 160, 22, 16, p.bodyText);
  text("KEY", 386, 8, 52, 14, 9, p.bodyText, juce::Justification::centredLeft);
  text("SCALE", 450, 8, 154, 14, 9, p.bodyText,
       juce::Justification::centredLeft);
  text("INPUT TYPE", 234, 8, 140, 14, 9, p.bodyText,
       juce::Justification::centredLeft);
  panel({24, 380, 652, 64}, p.bgDark, 9);
  text("ORIGINAL", 270, 386, 68, 24, 10, p.heatCold);
  text("TARGET", 380, 386, 56, 24, 10, p.heatWarm);
  text("A4", 490, 386, 30, 24, 10, p.textDim);
  for (int midi : {36, 84}) {
    const float y = 436 - (midi - 36) * 18 / 48.0f;
    g.setColour(p.accentDeep.withAlpha(.6f));
    g.drawLine(63, y, 660, y, .7f);
    text("C" + juce::String(midi / 12 - 1), 32, static_cast<int>(y) - 6, 28, 12,
         9, p.textDim);
  }
  text("ALLOWED NOTES", 30, 460, 300, 18, 10, p.bodyText.withAlpha(.75f), juce::Justification::centredLeft);
  panel({24, 490, 652, 170}, p.bgDark, 8);
}

void HumHouseVocalTuneEditor::paint(juce::Graphics &g) {
  if (!backdrop.isValid()) {
    const float displayScale = juce::Component::getApproximateScaleFactorForComponent(this);
    const float rasterScale = juce::jlimit(1.0f, 1.5f, displayScale);
    backdrop = juce::Image(juce::Image::RGB, juce::roundToInt(getWidth() * rasterScale),
                           juce::roundToInt(getHeight() * rasterScale), true);
    juce::Graphics imageGraphics(backdrop);
    imageGraphics.addTransform(juce::AffineTransform::scale(rasterScale));
    paintBackdrop(imageGraphics);
  }
  g.drawImage(backdrop, getLocalBounds().toFloat());
  g.addTransform(juce::AffineTransform::scale(scaleFactor()));
  if (!g.getClipBounds().intersects(juce::Rectangle<int>(24, 386, 652, 58)))
    return;
  const auto &p = lnf.palette;
  const auto drawNote = [&](int midi, int x, juce::Colour colour) {
    if (midi < 0) return;
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
    g.drawText(kNoteNames[midi % 12] + juce::String(midi / 12 - 1),
               x, 386, 34, 24, juce::Justification::centred);
  };
  drawNote(displayDetectedMidi, 342, p.heatCold);
  drawNote(displayTargetMidi, 439, p.heatWarm);
  if (historyFrames == 0) return;
  const auto pitchY = [](float hz) {
    const float midi = 69 + 12 * std::log2(juce::jmax(1.0f, hz) / 440.0f);
    return juce::jlimit(418.0f, 436.0f, 436 - (midi - 36) * 18 / 48);
  };
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
  // Draw the wider target first so both colours remain visible where notes agree.
  g.setColour(p.heatWarm.withAlpha(.88f));
  g.strokePath(target, juce::PathStrokeType(3.1f));
  g.setColour(p.heatCold);
  g.strokePath(sung, juce::PathStrokeType(1.25f));

}


void HumHouseVocalTuneEditor::paintOverChildren(juce::Graphics &g) {
  g.addTransform(juce::AffineTransform::scale(scaleFactor()));
  if (g.getClipBounds().intersects(juce::Rectangle<int>(148, 449, 511, 37))) {
    const float travel = orb.reducedMotion ? 0.0f : static_cast<float>(orb.phase);
    constexpr std::array<juce::uint32, 9> colours{
        0xff8effaa, 0xff71d7ff, 0xffb996ff, 0xfff4bf69, 0xffff8aae,
        0xff83ebda, 0xffffab73, 0xffaaaaff, 0xffd6fc79};
    const float energy = orb.reducedMotion ? 0.0f : orb.activity;
    constexpr int runeCount = 42;
    for (int i = 0; i < runeCount; ++i) {
      const float slot = static_cast<float>(i) / runeCount;
      float cluster = 0.0f;
      for (int j = 0; j < 3; ++j) {
        const float head = travel * (.025f + .014f * j) + j / 3.0f;
        const float distance = std::abs(std::remainder(slot - head, 1.0f));
        cluster = std::max(cluster, std::max(0.0f, 1.0f - distance * 13.0f));
      }
      const float twinkle = std::max(0.0f, std::sin(travel * .83f + i * 2.399963f));
      const float response = orb.reducedMotion ? 0.0f
          : (.09f + .55f * energy) * cluster + .32f * energy * twinkle * twinkle * twinkle;
      const float x = 158.0f + i * 12.0f;
      const float y = 467.0f + (i % 3) * 3.0f;
      juce::Path bones;
      if (i % 2 == 0) {
        bones.addRoundedRectangle(x - 4, y - 6, 8, 8, 2.5f);
        bones.startNewSubPath(x - 2.5f, y + 2);
        bones.lineTo(x - 2.5f, y + 5);
        bones.lineTo(x + 2.5f, y + 5);
        bones.lineTo(x + 2.5f, y + 2);
        bones.addEllipse(x - 2.6f, y - 2.8f, 1.9f, 2.2f);
        bones.addEllipse(x + .7f, y - 2.8f, 1.9f, 2.2f);
      } else {
        bones.addRoundedRectangle(x - 5, y - 7, 10, 14, 2.0f);
        bones.startNewSubPath(x, y - 4);
        bones.lineTo(x, y + 5);
        for (int rib = -3; rib <= 3; rib += 3) {
          bones.startNewSubPath(x - 3, y + rib - 1);
          bones.lineTo(x, y + rib + 1);
          bones.lineTo(x + 3, y + rib - 1);
        }
      }
      const auto colour = juce::Colour(colours[static_cast<size_t>(i) % colours.size()]);
      if (response > .28f) {
        g.setColour(colour.withAlpha(.18f * response));
        g.strokePath(bones, juce::PathStrokeType(2.7f));
      }
      g.setColour(colour.withAlpha(.30f + .60f * response));
      g.strokePath(bones, juce::PathStrokeType(1.3f));
    }
  }
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
  backdrop = {};
  const float scale = scaleFactor();
  lnf.setScale(scale);
  auto rect = [scale](int x, int y, int w, int h) {
    return juce::Rectangle<int>(
        juce::roundToInt(x * scale), juce::roundToInt(y * scale),
        juce::roundToInt(w * scale), juce::roundToInt(h * scale));
  };
  speedKnob.setBounds(rect(40, 154, 152, 152));
  humanizeKnob.setBounds(rect(508, 154, 152, 152));
  retuneValue.setBounds(rect(40, 310, 152, 24));
  humanizeValue.setBounds(rect(508, 310, 152, 24));
  orb.setBounds(rect(215, 95, 270, 270));
  rootNoteBox.setBounds(rect(382, 26, 62, 30));
  scaleBox.setBounds(rect(450, 26, 154, 30));
  referenceFreqKnob.setBounds(rect(550, 386, 86, 24));
  referenceDown.setBounds(rect(522, 387, 24, 22));
  referenceUp.setBounds(rect(640, 387, 24, 22));
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
