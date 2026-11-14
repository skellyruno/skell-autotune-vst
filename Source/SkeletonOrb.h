#pragma once
#include <JuceHeader.h>
#include <cmath>

class SkeletonOrb : public juce::Component {
  public:
    SkeletonOrb() { setInterceptsMouseClicks(false, false); }
    juce::Colour accent{0xff86dc50}, metalHigh{0xffdeded3}, metalLow{0xff777b6e};
    float activity = 0;
    float motionLevel = 0;
    double phase = 0;
    double sweepPhase = 0;
    float intensity = 1;
    bool bypassed = false;
    bool reducedMotion = false;
    void paint(juce::Graphics &graphics) override {
        juce::Graphics::ScopedSaveState saved(graphics);
        graphics.addTransform(juce::AffineTransform::scale(getWidth() / 280.0f, getHeight() / 280.0f));
        auto &g = graphics;
        if (!shell.isValid() || cachedAccent != accent || cachedHigh != metalHigh || cachedLow != metalLow) {
            const auto density = juce::jlimit(1.0f, 1.5f,
                juce::Component::getApproximateScaleFactorForComponent(this));
            shell = juce::Image(juce::Image::ARGB, juce::roundToInt(getWidth() * density),
                                juce::roundToInt(getHeight() * density), true);
            juce::Graphics shellGraphics(shell);
            shellGraphics.addTransform(juce::AffineTransform::scale(shell.getWidth() / 280.0f,
                                                                    shell.getHeight() / 280.0f));
            paintShell(shellGraphics);
            cachedAccent = accent;
            cachedHigh = metalHigh;
            cachedLow = metalLow;
        }
        g.drawImage(shell, juce::Rectangle<float>(0, 0, 280, 280));
        if (bypassed) {
            g.setColour(juce::Colour(0xff070a08));
            g.fillEllipse(17, 17, 246, 246);
            return;
        }
        {
            juce::Graphics::ScopedSaveState clipped(g);
            juce::Path clip;
            clip.addEllipse(18, 18, 244, 244);
            g.reduceClipRegion(clip);
            // Rotate longitude on a tilted sphere, leaving the rim and reflection fixed.
            const float rotation = reducedMotion ? 0.f : static_cast<float>(sweepPhase);
            const float tilt = -.22f;
            for (int meridian = 0; meridian < 10; ++meridian) {
                const float longitude = rotation + meridian * juce::MathConstants<float>::twoPi / 10;
                const float depth = std::cos(longitude);
                if (depth <= 0) continue;
                juce::Path line;
                for (int step = 0; step <= 20; ++step) {
                    const float latitude = (step / 20.f - .5f) * juce::MathConstants<float>::pi;
                    const float x = std::cos(latitude) * std::sin(longitude);
                    const float y = std::sin(latitude) * std::cos(tilt)
                                    + std::cos(latitude) * depth * std::sin(tilt);
                    if (step == 0) line.startNewSubPath(140 + 121 * x, 140 + 121 * y);
                    else line.lineTo(140 + 121 * x, 140 + 121 * y);
                }
                g.setColour(accent.withAlpha(.06f + .14f * depth));
                g.strokePath(line, juce::PathStrokeType(.8f));
            }
            if (!reducedMotion) for (int light = 0; light < 2; ++light) {
                const float travel = static_cast<float>(sweepPhase);
                const float longitude = travel * (light == 0 ? 1.0f : -2.0f) + light * 2.3f;
                const float latitude = 1.27f * std::sin((light == 0 ? 2 : 3) * travel + light * 1.9f);
                const float depth = std::cos(latitude) * std::cos(longitude);
                if (depth <= 0.0f)
                    continue;
                const float sweepX = 140 + 116 * std::cos(latitude) * std::sin(longitude);
                const float sweepY = 140 + 116 * std::sin(latitude);
                const auto neon = juce::Colour(0xff64ff32);
                juce::ColourGradient glow(neon.withAlpha(depth * (.11f + .20f * activity)), sweepX, sweepY,
                                          neon.withAlpha(0.0f), sweepX + 49, sweepY, true);
                g.setGradientFill(glow);
                g.fillEllipse(sweepX - 49, sweepY - 49, 98, 98);
                g.setColour(neon.withAlpha(depth * (.27f + .28f * activity)));
                g.fillEllipse(sweepX - 2, sweepY - 2, 4, 4);
            }
            juce::Graphics::ScopedSaveState skeleton(g);
            const float movement = reducedMotion ? 0.0f : .22f + .78f * motionLevel;
            g.addTransform(juce::AffineTransform::rotation(
                static_cast<float>(std::sin(phase) * .065 * movement), 140, 150).translated(0,
                    static_cast<float>(std::sin(phase * .5) * 5 * movement)));
            const auto boneColour = juce::Colour(0xffdbe8cc)
                                        .interpolatedWith(accent, .25f)
                                        .withAlpha(bypassed ? .3f : .85f + .15f * activity);
            auto bone = [&](float x, float y, float xx, float yy, float width = 2.0f) {
                const float recovery = .16f + .84f * std::pow(std::abs(std::sin(static_cast<float>(phase) * (.58f + .14f * std::sin(x)) + x * .091f + y * .023f)), 3.0f);
                const float energy = reducedMotion ? 0.0f : activity * intensity * recovery;
                const float identity = x * .17f + y * .071f + xx * .023f;
                const int pieces = energy > .01f && width > 1.5f ? 2 : 1;
                for (int segment = 0; segment < pieces; ++segment) {
                    const float u = static_cast<float>(segment) / pieces;
                    const float v = (segment + (energy > .01f ? .94f : 1.0f)) / pieces;
                    const float angle = static_cast<float>(phase) + identity + segment * 2.4f + .85f * std::sin(identity * 2.63f + segment + static_cast<float>(phase) * .71f);
                    const float orbitRadius = 28.0f + 78.0f * (.5f + .5f * std::sin(identity * 2.13f + segment));
                    const float scatter = juce::jlimit(0.0f, 1.0f, energy * (width > 1.5f ? 1.75f : 1.45f));
                    const float midX = x + (xx - x) * (u + v) * .5f;
                    const float midY = y + (yy - y) * (u + v) * .5f;
                    const float dx = scatter * (140 + orbitRadius * std::sin(angle) - midX);
                    const float dy = scatter * (140 + orbitRadius * std::cos(angle) - midY);
                    const float ax = x + (xx - x) * u + dx, ay = y + (yy - y) * u + dy;
                    const float bx = x + (xx - x) * v + dx, by = y + (yy - y) * v + dy;
                    if (scatter > .2f && width > 1.5f) {
                        g.setColour(accent.withAlpha(scatter * .28f));
                        g.drawLine(ax - 9 * scatter * std::cos(angle),
                                   ay + 9 * scatter * std::sin(angle), ax, ay, .9f);
                    }
                    g.setColour(boneColour.interpolatedWith(accent, .65f * scatter));
                    g.drawLine(ax, ay, bx, by, width);
                }
            };
            // Let the character keep moving gently between phrases.
            juce::Path skull;
            skull.startNewSubPath(124, 58);
            skull.cubicTo(120, 37, 160, 37, 156, 58);
            skull.lineTo(153, 65);
            skull.lineTo(149, 67);
            skull.lineTo(149, 76);
            skull.lineTo(131, 76);
            skull.lineTo(131, 67);
            skull.lineTo(127, 65);
            skull.closeSubPath();
            g.setGradientFill(juce::ColourGradient(boneColour.brighter(.2f), 126, 44, boneColour.darker(.5f),
                                                   155, 77, false));
            if (activity < .45f || reducedMotion)
                g.fillPath(skull);
            if (activity < .45f || reducedMotion) {
            g.setColour(juce::Colour(0xff092114));
            g.fillEllipse(128, 53, 9, 11);
            g.fillEllipse(143, 53, 9, 11);
            juce::Path nose;
            nose.addTriangle(140, 61, 137, 67, 143, 67);
            g.fillPath(nose);
            for (int i = 0; i < 5; ++i)
                g.drawLine(133.0f + i * 3.5f, 71, 133.0f + i * 3.5f, 76, .7f);
            g.setColour(boneColour);
            }
            bone(128, 73, 152, 73);
            for (int i = 0; i < 5; ++i)
                bone(132 + i * 4, 71, 132 + i * 4, 77, 1);
            for (int i = 0; i < 12; ++i)
                bone(137, 81 + i * 5, 143, 83 + i * 5, 1.2f);
            bone(140, 89, 113, 97);
            bone(140, 89, 167, 97);
            for (int i = 0; i < 5; ++i) {
                float y = 99 + i * 8.0f;
                for (int side : {-1, 1}) {
                    float px = 140, py = y;
                    for (int part = 1; part <= 7; ++part) {
                        const float t = part / 7.0f;
                        const float nx = 140 + side * 28 * std::sin(t * juce::MathConstants<float>::pi);
                        const float ny = y + 8 * t;
                        bone(px, py, nx, ny, 1.4f);
                        px = nx;
                        py = ny;
                    }
                }
            }
            const float sway = static_cast<float>(std::sin(phase * 1.2) * 6 * movement);
            bone(113, 97, 102, 139 + sway);
            bone(102, 139 + sway, 97, 177);
            bone(167, 97, 178, 139 - sway);
            bone(178, 139 - sway, 183, 177);
            for (int i = 0; i < 4; ++i) {
                bone(97 + i * 2, 177, 91 + i * 3, 190, 1);
                bone(183 - i * 2, 177, 189 - i * 3, 190, 1);
            }
            bone(124, 146, 156, 146);
            bone(156, 146, 150, 165);
            bone(150, 165, 140, 159);
            bone(140, 159, 130, 165);
            bone(130, 165, 124, 146);
            bone(130, 163, 124, 199);
            bone(124, 199, 119, 231);
            bone(150, 163, 156, 199);
            bone(156, 199, 161, 231);
            if (activity < .2f)
            for (auto point : {juce::Point<float>(102, 139 + sway), juce::Point<float>(178, 139 - sway),
                               juce::Point<float>(124, 199), juce::Point<float>(156, 199)})
                g.drawEllipse(point.x - 3, point.y - 3, 6, 6, 1.2f);
            bone(119, 231, 108, 237);
            bone(108, 237, 125, 237);
            bone(161, 231, 172, 237);
            bone(172, 237, 155, 237);
        }
    }

    void resized() override { shell = {}; }

  private:
    juce::Image shell;
    juce::Colour cachedAccent, cachedHigh, cachedLow;

    void paintShell(juce::Graphics &g) {
        g.setColour(juce::Colours::black.withAlpha(.20f));
        g.fillEllipse(5, 8, 270, 270);
        juce::ColourGradient rim(metalHigh, 35, 15, metalLow, 250, 265, false);
        g.setGradientFill(rim);
        g.fillEllipse(5, 5, 270, 270);
        g.setColour(juce::Colour(0xff07100b));
        g.fillEllipse(11, 11, 258, 258);
        juce::ColourGradient glass(juce::Colour(0xff346a46), 96, 74, juce::Colour(0xff010302), 238, 243,
                                   true);
        glass.addColour(.36, juce::Colour(0xff153723));
        glass.addColour(.68, juce::Colour(0xff081b10));
        g.setGradientFill(glass);
        g.fillEllipse(18, 18, 244, 244);
        juce::Path clip;
        clip.addEllipse(18, 18, 244, 244);
        {
            juce::Graphics::ScopedSaveState clipped(g);
            g.reduceClipRegion(clip);
            g.setColour(accent.withAlpha(.10f));
            // Cache the projected grid and glass shading once per size and theme.
            auto project = [&](float latitude, float longitude) {
                const float angle = longitude + 0.35f;
                const float x = std::cos(latitude) * std::sin(angle);
                const float z = std::cos(latitude) * std::cos(angle);
                const float y = std::sin(latitude);
                return juce::Point<float>(140 + 120 * x, 140 + 116 * y + 18 * z);
            };
            for (int latitude = -2; latitude <= 2; ++latitude) {
                juce::Path line;
                for (int sample = 0; sample <= 64; ++sample) {
                    auto point = project(latitude * .48f, sample * 6.283185f / 64);
                    if (sample == 0)
                        line.startNewSubPath(point);
                    else
                        line.lineTo(point);
                }
                g.setColour(accent.withAlpha(.138f));
                g.strokePath(line, juce::PathStrokeType(.8f));
            }
        }
        g.setColour(accent.withAlpha(.6325f));
        g.drawEllipse(15, 15, 250, 250, 1.4f);
        juce::ColourGradient reflection(juce::Colours::white.withAlpha(.31f), 65, 42,
                                        juce::Colours::white.withAlpha(0.0f), 111, 110, false);
        g.setGradientFill(reflection);
        juce::Path shine;
        shine.addEllipse(45, 36, 112, 38);
        shine.applyTransform(juce::AffineTransform::rotation(-.55f, 102, 80));
        g.fillPath(shine);
        juce::Path edge;
        edge.addCentredArc(140, 140, 127, 127, 0, -1.1f, .3f, true);
        g.setColour(juce::Colours::white.withAlpha(.345f));
        g.strokePath(edge, juce::PathStrokeType(1.2f));
    }
};
