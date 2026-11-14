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
    double globePhase = 0;
    float intensity = 1;
    bool bypassed = false;
    bool reducedMotion = false;
    void paint(juce::Graphics &graphics) override {
        juce::Graphics::ScopedSaveState saved(graphics);
        graphics.addTransform(juce::AffineTransform::scale(getWidth() / 280.0f, getHeight() / 280.0f));
        auto &g = graphics;
        g.setColour(juce::Colours::black.withAlpha(.20f));
        g.fillEllipse(5, 8, 270, 270);
        juce::ColourGradient rim(metalHigh, 35, 15, metalLow, 250, 265, false);
        g.setGradientFill(rim);
        g.fillEllipse(5, 5, 270, 270);
        g.setColour(juce::Colour(0xff07100b));
        g.fillEllipse(11, 11, 258, 258);
        if (bypassed) {
            g.setColour(juce::Colour(0xff070a08));
            g.fillEllipse(18, 18, 244, 244);
            return;
        }
        juce::ColourGradient glass(juce::Colour(0xff1d3d27), 93, 58, juce::Colour(0xff030a06), 235, 243,
                                   true);
        glass.addColour(.4, juce::Colour(0xff0c2114));
        g.setGradientFill(glass);
        g.fillEllipse(18, 18, 244, 244);
        juce::Path clip;
        clip.addEllipse(18, 18, 244, 244);
        {
            juce::Graphics::ScopedSaveState clipped(g);
            g.reduceClipRegion(clip);
            g.setColour(accent.withAlpha(.10f));
            // A rotating spherical grid uses true 3D projection and depth shading.
            auto project = [&](float latitude, float longitude) {
                const float angle = longitude + static_cast<float>(globePhase);
                const float x = std::cos(latitude) * std::sin(angle);
                const float z = std::cos(latitude) * std::cos(angle);
                const float y = std::sin(latitude);
                return juce::Point<float>(140 + 120 * x, 140 + 116 * y + 18 * z);
            };
            for (int meridian = 0; meridian < 10; ++meridian) {
                juce::Path line;
                for (int sample = 0; sample <= 48; ++sample) {
                    auto point = project(-1.57f + sample * 3.14f / 48, meridian * 6.283185f / 10);
                    if (sample == 0)
                        line.startNewSubPath(point);
                    else
                        line.lineTo(point);
                }
                g.setColour(accent.withAlpha(
                    .08f +
                    .06f * std::max(0.0f, std::cos(static_cast<float>(globePhase) + meridian * .6283f))));
                g.strokePath(line, juce::PathStrokeType(.8f));
            }
            for (int latitude = -2; latitude <= 2; ++latitude) {
                juce::Path line;
                for (int sample = 0; sample <= 64; ++sample) {
                    auto point = project(latitude * .48f, sample * 6.283185f / 64);
                    if (sample == 0)
                        line.startNewSubPath(point);
                    else
                        line.lineTo(point);
                }
                g.setColour(accent.withAlpha(.12f));
                g.strokePath(line, juce::PathStrokeType(.8f));
            }
            juce::Graphics::ScopedSaveState skeleton(g);
            const float movement = reducedMotion ? 0.0f : motionLevel;
            g.addTransform(juce::AffineTransform::rotation(
                static_cast<float>(std::sin(phase) * .035 * movement), 140, 150));
            const auto boneColour = juce::Colour(0xffdbe8cc)
                                        .interpolatedWith(accent, .25f)
                                        .withAlpha(bypassed ? .3f : .85f + .15f * activity);
            auto bone = [&](float x, float y, float xx, float yy, float width = 2.0f) {
                const float energy = reducedMotion ? 0.0f : activity * intensity;
                const float identity = x * .17f + y * .071f + xx * .023f;
                for (int segment = 0, pieces = energy < .01f ? 1 : (width > 1.5f ? 5 : 2); segment < pieces; ++segment) {
                    const float u = segment / static_cast<float>(pieces), v = (segment + (energy < .01f ? 1.0f : .86f)) / static_cast<float>(pieces);
                    const float pulse =
                        std::max(0.0f, static_cast<float>(std::sin(phase * 2.7 + identity + segment * 1.9)));
                    const float fracture = energy * pulse * pulse;
                    const float dx = fracture * 15 * std::sin(identity + segment * 2.4f);
                    const float dy = fracture * 11 * std::cos(identity + segment * 1.3f);
                    const float ax = x + (xx - x) * u + dx, ay = y + (yy - y) * u + dy;
                    const float bx = x + (xx - x) * v + dx, by = y + (yy - y) * v + dy;
                    if (fracture > .10f) {
                        g.setColour(accent.withAlpha(fracture * .22f));
                        g.drawLine(ax, ay, bx, by, width + 3);
                    }
                    g.setColour(boneColour.withMultipliedAlpha(1 - fracture * .4f));
                    g.drawLine(ax, ay, bx, by, width);
                    if (fracture > .12f) {
                        g.setColour(accent.withAlpha(fracture * .7f));
                        g.fillRect(ax - 1, ay - 1, 1.8f, 1.8f);
                        g.drawLine(ax, ay, ax, ay + 3 + fracture * 8, .6f);
                    }
                }
            };
            // An articulated character; its movement follows voiced activity only.
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
            g.fillPath(skull);
            g.setColour(juce::Colour(0xff092114));
            g.fillEllipse(128, 53, 9, 11);
            g.fillEllipse(143, 53, 9, 11);
            juce::Path nose;
            nose.addTriangle(140, 61, 137, 67, 143, 67);
            g.fillPath(nose);
            for (int i = 0; i < 5; ++i)
                g.drawLine(133.0f + i * 3.5f, 71, 133.0f + i * 3.5f, 76, .7f);
            g.setColour(boneColour);
            bone(128, 73, 152, 73);
            for (int i = 0; i < 5; ++i)
                bone(132 + i * 4, 71, 132 + i * 4, 77, 1);
            for (int i = 0; i < 12; ++i)
                g.drawEllipse(137, 81 + i * 5, 6, 4, 1.2f);
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
            juce::Path pelvis;
            pelvis.startNewSubPath(124, 146);
            pelvis.lineTo(156, 146);
            pelvis.lineTo(150, 165);
            pelvis.lineTo(140, 159);
            pelvis.lineTo(130, 165);
            pelvis.closeSubPath();
            g.strokePath(pelvis, juce::PathStrokeType(2));
            bone(130, 163, 124, 199);
            bone(124, 199, 119, 231);
            bone(150, 163, 156, 199);
            bone(156, 199, 161, 231);
            for (auto point : {juce::Point<float>(102, 139 + sway), juce::Point<float>(178, 139 - sway),
                               juce::Point<float>(124, 199), juce::Point<float>(156, 199)})
                g.drawEllipse(point.x - 3, point.y - 3, 6, 6, 1.2f);
            bone(119, 231, 108, 237);
            bone(108, 237, 125, 237);
            bone(161, 231, 172, 237);
            bone(172, 237, 155, 237);
        }
        g.setColour(accent.withAlpha(bypassed ? .15f : .55f));
        g.drawEllipse(15, 15, 250, 250, 1.4f);
        juce::ColourGradient reflection(juce::Colours::white.withAlpha(.27f), 65, 42,
                                        juce::Colours::white.withAlpha(0.0f), 111, 110, false);
        g.setGradientFill(reflection);
        juce::Path shine;
        shine.addEllipse(45, 36, 112, 38);
        shine.applyTransform(juce::AffineTransform::rotation(-.55f, 102, 80));
        g.fillPath(shine);
        juce::Path edge;
        edge.addCentredArc(140, 140, 127, 127, 0, -1.1f, .3f, true);
        g.setColour(juce::Colours::white.withAlpha(.3f));
        g.strokePath(edge, juce::PathStrokeType(1.2f));
    }
};
