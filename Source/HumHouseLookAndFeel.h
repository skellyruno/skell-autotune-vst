#pragma once
#include <JuceHeader.h>
#include <cmath>

namespace humtune {
struct Palette {
  juce::Colour body{0xffdedbd1}, surface{0xffebe8df}, edge{0xffaba99e},
      bodyText{0xff252b26};
  juce::Colour metalHigh{0xffe4e4dc}, metalLow{0xff787e77};
  juce::Colour bgDark{0xff080e0b}, bgDeep{0xff030806}, bgPanel{0xff101713},
      bgSection{0xff18221c};
  juce::Colour accent{0xff75ce40}, accentBright{0xffacfa78},
      accentDim{0xff3b713a}, accentDeep{0xff1b3320};
  juce::Colour textBright{0xffedf2e9}, textDim{0xffa0afa4},
      textValue{0xffb8fa95};
  juce::Colour heatCold{0xffdbe9df}, heatWarm{0xff8fe85e};
};
class HumHouseLookAndFeel : public juce::LookAndFeel_V4 {
public:
  Palette palette;
  float illumination = .6f, flare = 0, lightPhase = 0;
  HumHouseLookAndFeel() { setTheme(0); }
  void setScale(float value) { scale = juce::jmax(0.1f, value); }
  float getScale() const { return scale; }
  void setTheme(int theme) {
    palette = Palette{};
    if (theme == 1) {
      palette.body = juce::Colour(0xff060807);
      palette.surface = juce::Colour(0xff0b0f0d);
      palette.edge = juce::Colour(0xff303a33);
      palette.bodyText = juce::Colour(0xffe1e7df);
      palette.metalHigh = juce::Colour(0xffced2ce);
      palette.metalLow = juce::Colour(0xff676d68);
    } else if (theme == 2) {
      palette.body = juce::Colour(0xff041007);
      palette.surface = juce::Colour(0xff0a2110);
      palette.edge = juce::Colour(0xff285c35);
      palette.bodyText = juce::Colour(0xffdcffdc);
      palette.accent = juce::Colour(0xff48ff67);
      palette.accentBright = juce::Colour(0xffb0ff86);
      palette.accentDim = juce::Colour(0xff287b3e);
      palette.accentDeep = juce::Colour(0xff0a3116);
      palette.heatWarm = palette.accentBright;
      palette.textValue = palette.accentBright;
    } else if (theme == 3) {
      palette.body = juce::Colour(0xffc9c1ae);
      palette.surface = juce::Colour(0xffddd5c3);
      palette.edge = juce::Colour(0xffa69c87);
      palette.bodyText = juce::Colour(0xff292c23);
      palette.metalHigh = juce::Colour(0xffe1dac7);
      palette.metalLow = juce::Colour(0xff817d6d);
      palette.accent = juce::Colour(0xffa1ca48);
      palette.accentBright = juce::Colour(0xffc5ef75);
      palette.heatWarm = palette.accentBright;
    }
    setColour(juce::Label::textColourId, palette.textBright);
    setColour(juce::ComboBox::textColourId, palette.textBright);
    setColour(juce::ComboBox::backgroundColourId, palette.bgDark);
    setColour(juce::ComboBox::outlineColourId, palette.edge);
    setColour(juce::PopupMenu::backgroundColourId, palette.bgPanel);
    setColour(juce::PopupMenu::textColourId, palette.textBright);
    setColour(juce::PopupMenu::highlightedBackgroundColourId,
              palette.accentDeep);
    setColour(juce::PopupMenu::highlightedTextColourId, palette.textBright);
    setColour(juce::TooltipWindow::backgroundColourId, palette.bgPanel);
    setColour(juce::TooltipWindow::textColourId, palette.textBright);
    setColour(juce::TooltipWindow::outlineColourId, palette.edge);
  }
  void drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h,
                        float position, float start, float end,
                        juce::Slider &slider) override {
    const float cx = x + w * .5f, cy = y + h * .5f, r = juce::jmin(w, h) * .46f;
    const float glow =
        slider.isEnabled()
            ? std::clamp(.4f + illumination * .45f + flare * .3f, 0.0f, 1.0f)
            : 0.0f;
    juce::ColourGradient halo(palette.accent.withAlpha(.25f * glow), cx, cy,
                              palette.accent.withAlpha(0.0f),
                              cx + r + 5 * scale, cy, true);
    g.setGradientFill(halo);
    g.fillEllipse(cx - r - 5 * scale, cy - r - 5 * scale, 2 * r + 10 * scale,
                  2 * r + 10 * scale);
    g.setColour(juce::Colours::black.withAlpha(.20f));
    g.fillEllipse(cx - r - 3, cy - r + 5, 2 * r + 6, 2 * r + 6);
    juce::ColourGradient bezel(palette.metalHigh, cx - r, cy - r,
                               palette.metalLow, cx + r, cy + r, false);
    g.setGradientFill(bezel);
    g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
    g.setColour(palette.bgDark);
    g.fillEllipse(cx - r + 3 * scale, cy - r + 3 * scale, 2 * r - 6 * scale,
                  2 * r - 6 * scale);
    const float arcRadius = r * .88f;
    juce::Path track;
    track.addCentredArc(cx, cy, arcRadius, arcRadius, 0, start, end, true);
    g.setColour(palette.accent.withAlpha(
        slider.isEnabled() ? .14f + .18f * illumination + .24f * flare : 0.0f));
    g.strokePath(track, juce::PathStrokeType(8 * scale));
    g.setColour(slider.isEnabled() ? palette.accentDeep
                                   : palette.textDim.darker(.7f));
    g.strokePath(track, juce::PathStrokeType(4 * scale));
    juce::Path arc;
    arc.addCentredArc(cx, cy, arcRadius, arcRadius, 0, start,
                      start + position * (end - start), true);
    g.setColour(palette.accent.withAlpha(slider.isEnabled() ? .18f + .18f * glow
                                                            : 0.0f));
    g.strokePath(arc,
                 juce::PathStrokeType(10 * scale, juce::PathStrokeType::curved,
                                      juce::PathStrokeType::rounded));
    g.setColour(slider.isEnabled() ? palette.accentBright : palette.textDim);
    g.strokePath(arc,
                 juce::PathStrokeType(4 * scale, juce::PathStrokeType::curved,
                                      juce::PathStrokeType::rounded));
    const float face = r * .78f;
    juce::ColourGradient metal(palette.metalHigh, cx - face * .6f, cy - face,
                               palette.metalLow, cx + face * .6f, cy + face,
                               false);
    metal.addColour(.47, palette.metalHigh.darker(.13f));
    g.setGradientFill(metal);
    g.fillEllipse(cx - face, cy - face, 2 * face, 2 * face);
    for (int i = 0; i < 72; ++i) {
      const float angle = i * juce::MathConstants<float>::twoPi / 72;
      g.setColour(juce::Colours::white.withAlpha(i % 2 == 0 ? .05f : .015f));
      g.drawLine(cx + std::sin(angle) * face * .24f,
                 cy - std::cos(angle) * face * .24f,
                 cx + std::sin(angle) * face * .96f,
                 cy - std::cos(angle) * face * .96f, .7f * scale);
    }
    g.setColour(palette.metalHigh);
    g.drawEllipse(cx - face, cy - face, 2 * face, 2 * face, 1.1f * scale);
    // Fine grip cuts follow the machined rim without competing with the
    // position marker.
    for (int i = 0; i < 64; ++i) {
      const float a = i * juce::MathConstants<float>::twoPi / 64;
      g.setColour(palette.metalLow.withAlpha(.45f));
      g.drawLine(cx + std::sin(a) * face * .94f, cy - std::cos(a) * face * .94f,
                 cx + std::sin(a) * face * .985f,
                 cy - std::cos(a) * face * .985f, .8f * scale);
    }
    g.setColour(juce::Colours::black.withAlpha(.45f));
    g.drawEllipse(cx - face + 2 * scale, cy - face + 2 * scale,
                  2 * face - 4 * scale, 2 * face - 4 * scale, .7f * scale);
    const float angle = start + position * (end - start);
    g.setColour(palette.textBright);
    g.drawLine(cx + std::sin(angle) * r * .76f, cy - std::cos(angle) * r * .76f,
               cx + std::sin(angle) * r * .94f, cy - std::cos(angle) * r * .94f,
               3 * scale);
    if (!static_cast<bool>(
            slider.getProperties().getWithDefault("showDragValue", false))) {
      const float hub = face * .2f;
      g.setColour(palette.metalLow.withAlpha(.24f));
      g.drawEllipse(cx - hub, cy - hub, hub * 2, hub * 2, .7f * scale);
      g.setColour(juce::Colours::white.withAlpha(.2f));
      g.drawEllipse(cx - hub + scale, cy - hub + scale, hub * 2 - 2 * scale,
                    hub * 2 - 2 * scale, .6f * scale);
    }
    if (static_cast<bool>(
            slider.getProperties().getWithDefault("showDragValue", false)) &&
        slider.isEnabled()) {
      const bool flipped = static_cast<bool>(
          slider.getProperties().getWithDefault("displayFlip", false));
      const double value =
          (flipped ? 1 - slider.getValue() : slider.getValue()) *
          static_cast<double>(
              slider.getProperties().getWithDefault("displayScale", 1.0));
      const juce::String text =
          juce::String(juce::roundToInt(value)) + slider.getTextValueSuffix();
      g.setColour(juce::Colour(0xff17201a));
      g.setFont(juce::Font(juce::FontOptions(26 * scale).withStyle("Bold")));
      g.drawText(text,
                 juce::Rectangle<float>(cx - face, cy - 20 * scale, 2 * face,
                                        40 * scale)
                     .toNearestInt(),
                 juce::Justification::centred, false);
    }
    if (flare > .02f && slider.isEnabled()) {
      juce::Path glint;
      const float sweep =
          std::fmod(lightPhase, juce::MathConstants<float>::twoPi);
      glint.addCentredArc(cx, cy, r * .98f, r * .98f, 0, sweep, sweep + .8f,
                          true);
      g.setColour(palette.accentBright.withAlpha(flare * .75f));
      g.strokePath(glint, juce::PathStrokeType(2 * scale));
    }
    if (slider.isMouseOverOrDragging() || slider.hasKeyboardFocus(true)) {
      g.setColour(palette.accent);
      g.drawEllipse(cx - r - 2 * scale, cy - r - 2 * scale, 2 * r + 4 * scale,
                    2 * r + 4 * scale, 1 * scale);
    }
  }
  juce::Button *createSliderButton(juce::Slider &, bool up) override {
    auto *button = new juce::TextButton(up ? "+" : "-");
    button->getProperties().set("referenceStep", true);
    return button;
  }
  void drawToggleButton(juce::Graphics &g, juce::ToggleButton &button,
                        bool hover, bool) override {
    auto b = button.getLocalBounds().toFloat().reduced(2 * scale);
    const bool on = button.getToggleState();
    if (static_cast<bool>(
            button.getProperties().getWithDefault("powerButton", false))) {
      juce::ColourGradient rim(palette.metalHigh, b.getX(), b.getY(),
                               palette.metalLow, b.getRight(), b.getBottom(),
                               false);
      g.setGradientFill(rim);
      g.fillEllipse(b);
      b.reduce(3 * scale, 3 * scale);
      g.setColour(palette.bgDark);
      g.fillEllipse(b);
      g.setColour(on ? palette.accentBright : palette.textDim);
      g.drawEllipse(b, 1.5f * scale);
      const float cx = b.getCentreX(), cy = b.getCentreY(),
                  r = b.getWidth() * .26f;
      juce::Path p;
      p.addCentredArc(cx, cy, r, r, 0, .65f,
                      juce::MathConstants<float>::twoPi - .65f, true);
      g.strokePath(p, juce::PathStrokeType(2.2f * scale,
                                           juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
      g.drawLine(cx, cy - r - 2 * scale, cx, cy - 1 * scale, 2.2f * scale);
      return;
    }
    if (static_cast<bool>(
            button.getProperties().getWithDefault("modeButton", false))) {
      g.setColour(on ? palette.bgSection : palette.bgDark);
      g.fillRoundedRectangle(b, 3 * scale);
      g.setColour(on ? palette.accent : palette.edge);
      g.drawRoundedRectangle(b, 3 * scale, .8f * scale);
      g.setColour(on ? palette.accentBright : palette.textBright);
      g.setFont(juce::Font(juce::FontOptions(9 * scale).withStyle("Bold")));
      g.drawText(button.getButtonText(), b.toNearestInt(),
                 juce::Justification::centred, false);
      return;
    }
    juce::ColourGradient face(on ? palette.accentDeep : palette.bgSection,
                              b.getX(), b.getY(), palette.bgDark, b.getX(),
                              b.getBottom(), false);
    g.setGradientFill(face);
    g.fillRoundedRectangle(b, 6 * scale);
    g.setColour(on || hover ? palette.accent : palette.edge);
    g.drawRoundedRectangle(b, 6 * scale, 1 * scale);
    g.setColour(on ? palette.accentBright : palette.textDim);
    g.fillEllipse(b.getX() + 10 * scale, b.getCentreY() - 3 * scale, 6 * scale,
                  6 * scale);
    g.setColour(palette.textBright);
    g.setFont(juce::Font(juce::FontOptions(12 * scale)));
    g.drawText(button.getButtonText(),
               b.withTrimmedLeft(24 * scale).toNearestInt(),
               juce::Justification::centred, false);
  }
  void drawButtonBackground(juce::Graphics &g, juce::Button &button,
                            const juce::Colour &, bool hover,
                            bool down) override {
    if (static_cast<bool>(
            button.getProperties().getWithDefault("settingsGear", false))) {
      const auto b = button.getLocalBounds().toFloat();
      const auto c = b.getCentre();
      juce::Path gear;
      for (int i = 0; i < 48; ++i) {
        const float angle = i * juce::MathConstants<float>::twoPi / 48;
        const float r =
            b.getWidth() * ((i % 6 == 0 || i % 6 == 5) ? .32f : .43f);
        const auto point =
            c + juce::Point<float>(std::sin(angle) * r, -std::cos(angle) * r);
        if (i == 0)
          gear.startNewSubPath(point);
        else
          gear.lineTo(point);
      }
      gear.closeSubPath();
      if (hover || button.hasKeyboardFocus(true)) {
        g.setColour(palette.bodyText.withAlpha(.08f));
        g.fillRoundedRectangle(b.reduced(scale), 5 * scale);
      }
      g.setColour(palette.bodyText);
      g.fillPath(gear);
      g.setColour(palette.body);
      g.fillEllipse(c.x - 4 * scale, c.y - 4 * scale, 8 * scale, 8 * scale);
      return;
    }
    if (static_cast<bool>(
            button.getProperties().getWithDefault("themeSkull", false))) {
      juce::Graphics::ScopedSaveState state(g);
      auto bounds = button.getLocalBounds().toFloat();
      g.addTransform(juce::AffineTransform::scale(bounds.getWidth() / 32,
                                                  bounds.getHeight() / 34));
      juce::Path skull;
      skull.startNewSubPath(5, 13);
      skull.cubicTo(3, 0, 29, 0, 27, 13);
      skull.lineTo(25, 20);
      skull.lineTo(22, 22);
      skull.lineTo(22, 29);
      skull.lineTo(10, 29);
      skull.lineTo(10, 22);
      skull.lineTo(7, 20);
      skull.closeSubPath();
      g.setGradientFill(juce::ColourGradient(palette.accentBright, 6, 3,
                                             palette.accentDim, 26, 30, false));
      g.fillPath(skull);
      g.setColour(palette.bgDark);
      g.fillEllipse(8, 11, 6, 8);
      g.fillEllipse(18, 11, 6, 8);
      juce::Path nose;
      nose.addTriangle(16, 17, 14, 22, 18, 22);
      g.fillPath(nose);
      for (float x : {12.0f, 16.0f, 20.0f})
        g.drawLine(x, 25, x, 29, .9f);
      if (button.isMouseOver()) {
        g.setColour(palette.accentBright);
        g.drawRoundedRectangle(1, 1, 30, 32, 5, 1);
      }
      return;
    }

    const auto b = button.getLocalBounds().toFloat().reduced(.8f * scale);
    const int key =
        static_cast<int>(button.getProperties().getWithDefault("pianoKey", 0));
    const bool target = static_cast<bool>(
        button.getProperties().getWithDefault("target", false));
    const bool allowed = button.getToggleState() && button.isEnabled();
    const bool black = key == 2;
    const float inset = down ? 2.0f * scale : 0.0f;
    auto face = b.withTrimmedTop(inset);
    juce::Colour top =
        black ? juce::Colour(0xff3c4640) : juce::Colour(0xfff8f7f0);
    juce::Colour bottom =
        black ? juce::Colour(0xff101713) : juce::Colour(0xffd2d5cd);
    if (target && button.isEnabled()) {
      top = top.interpolatedWith(palette.accentBright, .34f);
      bottom = bottom.interpolatedWith(palette.accent, .58f);
    }
    if (down)
      bottom = bottom.darker(.12f);
    g.setGradientFill(juce::ColourGradient(top, face.getX(), face.getY(),
                                           bottom, face.getX(),
                                           face.getBottom(), false));
    g.fillRoundedRectangle(face, 3 * scale);
    // A narrow lower lip and recessed edges give each key physical depth.
    g.setColour(black ? juce::Colour(0xff020704) : juce::Colour(0xffa4aea2));
    g.fillRoundedRectangle(
        face.withTop(face.getBottom() - (black ? 7 : 5) * scale), 2 * scale);
    g.setColour(juce::Colours::white.withAlpha(black ? .16f : .65f));
    g.drawLine(face.getX() + 3 * scale, face.getY() + 2 * scale,
               face.getRight() - 3 * scale, face.getY() + 2 * scale, scale);
    g.setColour(juce::Colours::black.withAlpha(black ? .6f : .14f));
    g.drawRoundedRectangle(face.reduced(.5f * scale), 3 * scale, scale);
    if (hover || button.hasKeyboardFocus(true)) {
      g.setColour(palette.accent.withAlpha(.85f));
      g.drawRoundedRectangle(face.reduced(scale), 3 * scale, 1.3f * scale);
    }
    if (key != 0) {
      const float y = face.getBottom() - 27 * scale;
      g.setColour(allowed ? (black ? palette.accentBright : palette.accentDim)
                          : (black ? juce::Colour(0xff435147)
                                   : juce::Colour(0xffa4ada1)));
      g.fillRoundedRectangle(face.getCentreX() - 5 * scale, y, 10 * scale,
                             2.5f * scale, scale);
    }
  }
  void drawButtonText(juce::Graphics &g, juce::TextButton &button, bool,
                      bool) override {
    const int key =
        static_cast<int>(button.getProperties().getWithDefault("pianoKey", 0));
    const bool target = static_cast<bool>(
        button.getProperties().getWithDefault("target", false));
    g.setColour(key == 2 && !(target && button.isEnabled())
                    ? palette.textBright
                    : juce::Colour(0xff253029));
    g.setFont(juce::Font(
        juce::FontOptions((key == 0 ? 12 : 13) * scale).withStyle("Bold")));
    auto b = button.getLocalBounds();
    if (key != 0)
      b = b.removeFromBottom(juce::roundToInt(23 * scale));
    g.drawText(button.getButtonText(), b, juce::Justification::centred, false);
  }
  void drawComboBox(juce::Graphics &g, int w, int h, bool, int, int, int, int,
                    juce::ComboBox &box) override {
    auto b = juce::Rectangle<float>(1, 1, w - 2.0f, h - 2.0f);
    g.setColour(palette.bgDark);
    g.fillRoundedRectangle(b, 6 * scale);
    g.setColour(box.isMouseOver(true) ? palette.accent : palette.edge);
    g.drawRoundedRectangle(b, 6 * scale, 1 * scale);
    juce::Path p;
    const float x = w - 14 * scale, y = h * .5f;
    p.addTriangle(x - 4 * scale, y - 2 * scale, x + 4 * scale, y - 2 * scale, x,
                  y + 3 * scale);
    g.setColour(palette.accent);
    g.fillPath(p);
  }
  juce::Font getPopupMenuFont() override {
    return juce::Font(juce::FontOptions(13 * scale));
  }
  void getIdealPopupMenuItemSize(const juce::String &, bool separator, int,
                                 int &width, int &height) override {
    width = juce::roundToInt(224 * scale);
    height = juce::roundToInt((separator ? 10 : 30) * scale);
  }
  void drawPopupMenuBackground(juce::Graphics &g, int w, int h) override {
    g.fillAll(palette.bgDark);
    auto b = juce::Rectangle<float>(1, 1, w - 2.f, h - 2.f);
    g.setColour(palette.bgPanel);
    g.fillRoundedRectangle(b, 8 * scale);
    g.setColour(palette.textDim.withAlpha(.25f));
    g.drawRoundedRectangle(b, 8 * scale, scale);
  }
  void drawPopupMenuSectionHeader(juce::Graphics &g,
                                  const juce::Rectangle<int> &area,
                                  const juce::String &text) override {
    g.setColour(palette.textDim);
    g.setFont(juce::Font(juce::FontOptions(11 * scale).withStyle("Bold")));
    g.drawText(text.toUpperCase(), area.reduced(14 * scale, 0),
               juce::Justification::centredLeft, false);
  }
  void drawPopupMenuItem(juce::Graphics &g, const juce::Rectangle<int> &area,
                         bool separator, bool active, bool highlight,
                         bool ticked, bool submenu, const juce::String &text,
                         const juce::String &, const juce::Drawable *,
                         const juce::Colour *) override {
    auto b = area.toFloat().reduced(6 * scale, 2 * scale);
    if (separator) {
      g.setColour(palette.textDim.withAlpha(.18f));
      g.drawHorizontalLine(area.getCentreY(), b.getX() + 8 * scale,
                           b.getRight() - 8 * scale);
      return;
    }
    if (highlight && active) {
      g.setColour(juce::Colours::white.withAlpha(.07f));
      g.fillRoundedRectangle(b, 5 * scale);
    }
    g.setColour(palette.textBright.withAlpha(active ? 1.f : .4f));
    g.setFont(getPopupMenuFont());
    g.drawText(text,
               b.withTrimmedLeft(9 * scale)
                   .withTrimmedRight(35 * scale)
                   .toNearestInt(),
               juce::Justification::centredLeft, false);
    auto check = juce::Rectangle<float>(b.getRight() - 25 * scale,
                                        b.getCentreY() - 8 * scale, 16 * scale,
                                        16 * scale);
    g.setColour(palette.textDim.withAlpha(.35f));
    g.drawRoundedRectangle(check, 3 * scale, scale);
    if (ticked) {
      juce::Path mark;
      mark.startNewSubPath(check.getX() + 3 * scale, check.getCentreY());
      mark.lineTo(check.getX() + 7 * scale, check.getBottom() - 4 * scale);
      mark.lineTo(check.getRight() - 3 * scale, check.getY() + 4 * scale);
      g.setColour(palette.accentBright);
      g.strokePath(mark, juce::PathStrokeType(1.8f * scale));
    }
    if (submenu) {
      g.setColour(palette.textDim);
      g.drawText(">", check.toNearestInt(), juce::Justification::centred,
                 false);
    }
  }
  juce::Font getComboBoxFont(juce::ComboBox &) override {
    return juce::Font(juce::FontOptions(12 * scale));
  }
  void positionComboBoxText(juce::ComboBox &box, juce::Label &label) override {
    label.setBounds(12 * scale, 1, box.getWidth() - 30 * scale,
                    box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
  }
  juce::Font getLabelFont(juce::Label &label) override {
    return juce::Font(juce::FontOptions(
        dynamic_cast<juce::Slider *>(label.getParentComponent()) ? 13 * scale
                                                                 : 12 * scale));
  }

private:
  float scale = 1;
};
} // namespace humtune
