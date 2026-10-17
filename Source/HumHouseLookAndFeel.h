#pragma once
#include <JuceHeader.h>

namespace humtune
{
// Neutral studio surfaces; colour is reserved for signal and active controls.
struct Palette
{
    static inline const juce::Colour bgDark { 0xff151719 }, bgDeep { 0xff101214 };
    static inline const juce::Colour bgPanel { 0xff1c1f22 }, bgSection { 0xff24282c };
    static inline const juce::Colour accent { 0xff8cdbb0 }, accentBright { 0xffb3f1cd };
    static inline const juce::Colour accentDim { 0xff3b4943 }, accentDeep { 0xff293b32 };
    static inline const juce::Colour textBright { 0xffedf0ef }, textDim { 0xff9ba5a2 }, textValue { 0xffe0e8e3 };
    static inline const juce::Colour heatCold { 0xff8ab7d4 }, heatWarm { 0xff8cdbb0 };
    static inline const juce::Colour meterGreen { 0xff8cdbb0 }, meterYellow { 0xffe7c58a }, meterRed { 0xffef8c86 };
    static inline const juce::Colour knobTrack { 0xff353b3e }, knobFill { 0xff8cdbb0 }, knobThumb { 0xffedf0ef };
};

class HumHouseLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HumHouseLookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId, Palette::bgDark);
        setColour(juce::Label::textColourId, Palette::textBright);
        setColour(juce::ComboBox::textColourId, Palette::textBright);
        setColour(juce::ComboBox::backgroundColourId, Palette::bgSection);
        setColour(juce::ComboBox::outlineColourId, Palette::knobTrack);
        setColour(juce::PopupMenu::backgroundColourId, Palette::bgPanel);
        setColour(juce::PopupMenu::textColourId, Palette::textBright);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, Palette::accentDeep);
        setColour(juce::PopupMenu::highlightedTextColourId, Palette::accentBright);
        setColour(juce::TooltipWindow::backgroundColourId, Palette::bgSection);
        setColour(juce::TooltipWindow::textColourId, Palette::textBright);
        setColour(juce::TooltipWindow::outlineColourId, Palette::knobTrack);
    }

    void setScale(float value) { scale = juce::jmax(0.1f, value); }
    static int pianoKeyType(juce::Component& c)
    {
        return static_cast<int>(c.getProperties().getWithDefault("pianoKey", 0));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float position, float start, float end, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                                  static_cast<float>(width), static_cast<float>(height)).reduced(6.0f * scale);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = start + position * (end - start);
        const bool hot = slider.isMouseOverOrDragging() || slider.hasKeyboardFocus(false);
        auto arc = [&](juce::Colour colour, float from, float to)
        {
            juce::Path path;
            path.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, from, to, true);
            g.setColour(colour);
            g.strokePath(path, juce::PathStrokeType(3.0f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        arc(Palette::knobTrack, start, end);
        if (position > 0.001f)
            arc(hot ? Palette::accentBright : Palette::accent, start, angle);

        const float inner = radius - 9.0f * scale;
        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillEllipse(centre.x - inner, centre.y - inner + 3.0f * scale, inner * 2, inner * 2);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff343a3e), centre.x, centre.y - inner,
                                              juce::Colour(0xff202427), centre.x, centre.y + inner, false));
        g.fillEllipse(centre.x - inner, centre.y - inner, inner * 2, inner * 2);
        g.setColour(hot ? Palette::accentDim : juce::Colour(0xff444c50));
        g.drawEllipse(centre.x - inner, centre.y - inner, inner * 2, inner * 2, scale);
        g.setColour(Palette::accentBright);
        g.drawLine(centre.x + inner * 0.76f * std::sin(angle), centre.y - inner * 0.76f * std::cos(angle),
                   centre.x + inner * 0.94f * std::sin(angle), centre.y - inner * 0.94f * std::cos(angle), 2.0f * scale);

        const double multiplier = static_cast<double>(slider.getProperties().getWithDefault("displayScale", 1.0));
        const bool flip = static_cast<bool>(slider.getProperties().getWithDefault("displayFlip", false));
        juce::String text;
        if (std::abs(multiplier - 1.0) > 0.00001)
            text = juce::String(juce::roundToInt((flip ? 1.0 - slider.getValue() : slider.getValue()) * multiplier));
        else if (slider.getProperties().contains("displayDecimals"))
            text = juce::String(slider.getValue(), static_cast<int>(slider.getProperties()["displayDecimals"]));
        else
            text = slider.getTextFromValue(slider.getValue());
        text += slider.getProperties().getWithDefault("displayUnit", "").toString();
        g.setColour(Palette::textBright);
        g.setFont(juce::Font(juce::FontOptions(juce::jmax(10.0f * scale, radius * 0.47f)).withStyle("Bold")));
        g.drawText(text, bounds.reduced(9.0f * scale).toNearestInt(), juce::Justification::centred, false);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool hot, bool down) override
    {
        auto b = button.getLocalBounds().toFloat().reduced(scale);
        const bool on = button.getToggleState();
        g.setColour(on ? Palette::accentDeep : Palette::bgSection);
        g.fillRoundedRectangle(b, 5.0f * scale);
        g.setColour(hot || button.hasKeyboardFocus(false) ? Palette::accent : Palette::knobTrack);
        g.drawRoundedRectangle(b, 5.0f * scale, scale);
        g.setColour(on ? Palette::accent : Palette::textDim.withAlpha(0.4f));
        g.fillEllipse(b.getX() + 10.0f * scale, b.getCentreY() - 2.5f * scale, 5.0f * scale, 5.0f * scale);
        g.setColour(on ? Palette::accentBright : Palette::textDim);
        g.setFont(juce::Font(juce::FontOptions(12.0f * scale)));
        g.drawText(button.getButtonText(), b.withTrimmedLeft(20.0f * scale).toNearestInt(), juce::Justification::centred, false);
        juce::ignoreUnused(down);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool hot, bool down) override
    {
        auto b = button.getLocalBounds().toFloat().reduced(0.7f * scale);
        const int type = pianoKeyType(button);
        const bool on = button.getToggleState();
        juce::Colour top, bottom;
        if (type == 1)
        {
            top = on ? juce::Colour(0xffd4ded8) : juce::Colour(0xff555e5a);
            bottom = on ? juce::Colour(0xffb5c5bc) : juce::Colour(0xff414946);
        }
        else if (type == 2)
        {
            top = juce::Colour(0xff333a37);
            bottom = juce::Colour(0xff141a17);
        }
        else
        {
            top = on ? Palette::accentDeep : Palette::bgSection;
            bottom = top;
        }
        if (hot || down) { top = top.brighter(0.1f); bottom = bottom.brighter(0.1f); }
        g.setGradientFill(juce::ColourGradient(top, 0, b.getY(), bottom, 0, b.getBottom(), false));
        g.fillRoundedRectangle(b, 4.0f * scale);
        g.setColour(hot || button.hasKeyboardFocus(false) ? Palette::accent : Palette::knobTrack);
        g.drawRoundedRectangle(b, 4.0f * scale, scale);
        if (type != 0 && on)
        {
            g.setColour(type == 1 ? juce::Colour(0xff428465) : Palette::accent);
            g.fillRoundedRectangle(b.getCentreX() - 9.0f * scale, b.getBottom() - 7.0f * scale, 18.0f * scale, 2.0f * scale, scale);
        }
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        const int type = pianoKeyType(button);
        auto b = button.getLocalBounds();
        g.setColour(type == 1 ? juce::Colour(0xff253c30) : Palette::textBright);
        if (type != 0 && !button.getToggleState())
            g.setColour(Palette::textDim);
        g.setFont(juce::Font(juce::FontOptions((type == 0 ? 11.5f : 12.0f) * scale).withStyle("Bold")));
        if (type != 0)
            b = b.removeFromBottom(juce::roundToInt(31.0f * scale));
        g.drawText(button.getButtonText(), b, juce::Justification::centred, false);
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto b = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height)).reduced(scale);
        g.setColour(Palette::bgSection);
        g.fillRoundedRectangle(b, 5.0f * scale);
        g.setColour(box.isMouseOver(true) || box.hasKeyboardFocus(true) ? Palette::accent : Palette::knobTrack);
        g.drawRoundedRectangle(b, 5.0f * scale, scale);
        const float ax = b.getRight() - 13.0f * scale, ay = b.getCentreY();
        juce::Path arrow;
        arrow.startNewSubPath(ax - 3.0f * scale, ay - scale);
        arrow.lineTo(ax, ay + 2.0f * scale);
        arrow.lineTo(ax + 3.0f * scale, ay - scale);
        g.setColour(Palette::textDim);
        g.strokePath(arrow, juce::PathStrokeType(1.3f * scale));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(juce::FontOptions(12.0f * scale)); }
    juce::Font getPopupMenuFont() override { return juce::Font(juce::FontOptions(13.0f * scale)); }
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(juce::roundToInt(11.0f * scale), 1, box.getWidth() - juce::roundToInt(32.0f * scale), box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
    }
private:
    float scale = 1.0f;
};
} // namespace humtune
