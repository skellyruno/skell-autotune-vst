#pragma once
#include <JuceHeader.h>
#include "SkeletonMotion.h"
#include <cmath>

class SkeletonOrb : public juce::Component {
  public:
    SkeletonOrb() { setInterceptsMouseClicks(false, false); }
    juce::Colour accent{0xff86dc50}, metalHigh{0xffdeded3}, metalLow{0xff777b6e};
    float activity = 0;
    float humanize = 0.0f; // visual morph only
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
                const auto neon = accent;
                juce::ColourGradient glow(neon.withAlpha(depth * (.11f + .20f * activity)), sweepX, sweepY,
                                          neon.withAlpha(0.0f), sweepX + 49, sweepY, true);
                g.setGradientFill(glow);
                g.fillEllipse(sweepX - 49, sweepY - 49, 98, 98);
                g.setColour(neon.withAlpha(depth * (.27f + .28f * activity)));
                g.fillEllipse(sweepX - 2, sweepY - 2, 4, 4);
            }
            // Sparse electrical discharges behind the bones, confined to glass.
            const float current = reducedMotion ? 0.0f
                : juce::jlimit(0.0f, 1.0f, activity * intensity);
            if (current > .10f) {
                const int bolts = current > .72f ? 3 : (current > .37f ? 2 : 1);
                for (int i = 0; i < bolts; ++i) {
                    const float p = static_cast<float>(phase);
                    const float x0 = i == 0 ? 69.0f : (i == 1 ? 207.0f : 192.0f);
                    const float y0 = i == 0 ? 93.0f : (i == 1 ? 94.0f : 166.0f);
                    const float direction = i == 1 ? -1.0f : 1.0f;
                    juce::Path arc;
                    arc.startNewSubPath(x0, y0);
                    for (int segment = 1; segment <= 6; ++segment)
                        arc.lineTo(x0 + direction * segment * 1.4f
                                       + 8.0f * std::sin(p * 2.35f + i * 2.13f + segment * 2.17f),
                                   y0 + segment * (i == 2 ? 9.0f : 12.0f));
                    const float power = current * (.68f + .32f * std::sin(p * 2.8f + i * 2.1f));
                    g.setColour(accent.withAlpha(.12f * power));
                    g.strokePath(arc, juce::PathStrokeType(6.0f));
                    g.setColour(accent.brighter(.3f).withAlpha(.42f * power));
                    g.strokePath(arc, juce::PathStrokeType(2.0f));
                    g.setColour(juce::Colours::white.withAlpha(.40f * power));
                    g.strokePath(arc, juce::PathStrokeType(.65f));
                    // One small, bounded branch gives the discharge a more
                    // convincing electrical silhouette without strobing.
                    const float forkY = y0 + (i == 2 ? 27.0f : 36.0f);
                    const float forkX = x0 + direction * 4.2f
                         + 8.0f * std::sin(p * 2.35f + i * 2.13f + 3 * 2.17f);
                    juce::Path fork;
                    fork.startNewSubPath(forkX, forkY);
                    fork.lineTo(forkX - direction * 12.0f, forkY + 7.0f);
                    fork.lineTo(forkX - direction * 7.0f, forkY + 16.0f);
                    fork.lineTo(forkX - direction * 20.0f, forkY + 24.0f);
                    g.setColour(accent.withAlpha(.25f * power));
                    g.strokePath(fork, juce::PathStrokeType(1.8f));
                    g.setColour(accent.brighter(.4f).withAlpha(.30f * power));
                    g.strokePath(fork, juce::PathStrokeType(.6f));
                }
            }
            juce::ColourGradient innerHalo(accent.withAlpha(.025f + .075f * activity),
                                           140, 146, accent.withAlpha(0.0f),
                                           216, 147, true);
            g.setGradientFill(innerHalo);
            g.fillEllipse(73, 70, 134, 170);
            juce::Graphics::ScopedSaveState skeleton(g);
            const float movement = reducedMotion ? 0.0f : .20f + .72f * motionLevel;
            g.addTransform(juce::AffineTransform::rotation(
                static_cast<float>(std::sin(phase) * .065 * movement), 140, 150).translated(0,
                    static_cast<float>(std::sin(phase * .5) * 5 * movement)));
            const auto boneColour = juce::Colour(0xffdbe8cc)
                                        .interpolatedWith(accent, .25f)
                                        .withAlpha(bypassed ? .3f : .85f + .15f * activity);
            const float h = juce::jlimit(0.0f, 1.0f, humanize);
            const float flesh = h * h * (3.0f - 2.0f * h);
            const auto skinLight = juce::Colour(0xffd8baa0).interpolatedWith(accent, .13f);
            const auto skinShadow = juce::Colour(0xff574337).interpolatedWith(accent, .13f);
            auto bone = [&](float x, float y, float xx, float yy, float width = 2.0f) {
                const auto scatter = skellytune::motion::boneScatter(
                    x, y, xx, yy, width, activity, intensity, phase, reducedMotion);
                const float drive = reducedMotion ? 0.0f
                    : juce::jlimit(0.0f, 1.0f, activity * intensity);
                // A faint anchored bone outline keeps the figure identifiable
                // while the brighter complete stroke scatters independently.
                if (drive > .20f && width > 1.5f && flesh < .99f) {
                    g.setColour(boneColour.withAlpha(.11f * drive * (1.0f - flesh)));
                    g.drawLine(x, y, xx, yy, width * .75f);
                }
                // Bare bones scatter exactly as before; flesh reconnects the joints.
                const float separation = (1.0f - flesh) * (1.0f - flesh);
                const float ax = x + scatter.x * separation, ay = y + scatter.y * separation;
                const float bx = xx + scatter.x * separation, by = yy + scatter.y * separation;
                if (flesh > .001f) {
                    const float muscle = (width > 1.5f ? 13.0f : 3.4f) * flesh;
                    juce::Path limb;
                    limb.startNewSubPath(ax, ay);
                    limb.lineTo(bx, by);
                    g.setColour(skinShadow.withAlpha(.93f * flesh));
                    g.strokePath(limb, juce::PathStrokeType(width + muscle,
                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    juce::ColourGradient skinVolume(skinLight.withAlpha(.96f * flesh), ax - 5.0f, ay,
                        skinShadow.withAlpha(.96f * flesh), ax + 9.0f, ay, false);
                    skinVolume.addColour(.36, skinLight.withAlpha(.96f * flesh));
                    g.setGradientFill(skinVolume);
                    g.strokePath(limb, juce::PathStrokeType(width + muscle * .63f,
                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                g.setColour(boneColour.interpolatedWith(accent, .17f * drive)
                    .withMultipliedAlpha(1.0f - .94f * flesh));
                g.drawLine(ax, ay, bx, by, width);
            };
            // The head bounces independently of the torso. Keeping the
            // face, jaw and teeth under one transform avoids detached eyes.
            auto head = skellytune::motion::headPose(
                activity, motionLevel, phase, reducedMotion);
            head.x *= 1.0f - .24f * flesh;
            head.y *= 1.0f - .40f * flesh;
            head.angle *= 1.0f - .20f * flesh;
            {
                juce::Graphics::ScopedSaveState headState(g);
                g.addTransform(
                    juce::AffineTransform::rotation(head.angle, 140, 70)
                        .translated(head.x, head.y));
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
            {
            g.setColour(juce::Colour(0xff092114));
            g.fillEllipse(128, 53, 9, 11);
            g.fillEllipse(143, 53, 9, 11);
            juce::Path nose;
            nose.addTriangle(140, 61, 137, 67, 143, 67);
            g.fillPath(nose);
            g.drawLine(127, 52, 135, 52, 1.0f);
            g.drawLine(145, 52, 153, 52, 1.0f);
            g.drawLine(131, 70, 149, 70, .8f);
            for (int i = 0; i < 5; ++i)
                g.drawLine(133.0f + i * 3.5f, 71, 133.0f + i * 3.5f, 76, .7f);
            g.setColour(boneColour);
            }
            // Skull details use the same transform; do not scatter its
            // jaw/teeth independently or the bounce looks like a glitch.
            g.setColour(boneColour.withAlpha(.9f));
            g.drawLine(128, 73, 152, 73, 1.6f);
            for (int i = 0; i < 5; ++i)
                g.drawLine(132 + i * 4, 71, 132 + i * 4, 77, 1.0f);
            if (flesh > .001f) {
                juce::Path face;
                face.startNewSubPath(140, 39);
                face.cubicTo(158, 39, 163, 52, 157, 64);
                face.cubicTo(154, 74, 149, 80, 140, 82);
                face.cubicTo(130, 80, 126, 74, 123, 64);
                face.cubicTo(117, 52, 122, 39, 140, 39);
                face.closeSubPath();
                juce::ColourGradient skin(skinLight.withAlpha(flesh), 128, 44,
                    skinShadow.withAlpha(flesh), 158, 77, false);
                skin.addColour(.48, skinLight.darker(.18f).withAlpha(flesh));
                g.setGradientFill(skin);
                g.fillPath(face);
                g.setColour(skinShadow.darker(.28f).withAlpha(.9f * flesh));
                g.fillEllipse(128.5f, 55, 7, 3.6f);
                g.fillEllipse(144.5f, 55, 7, 3.6f);
                g.drawLine(140, 59, 137.7f, 66, 1.3f);
                g.drawLine(137.7f, 66, 141.7f, 66, 1.1f);
                g.drawLine(134, 73, 146, 73, 1.2f);
                g.setColour(skinLight.brighter(.18f).withAlpha(.65f * flesh));
                g.drawLine(139, 57, 139, 64, 1.0f);
            }
            } // headState restores torso transform
            if (flesh > .001f) {
                juce::Path torso;
                torso.startNewSubPath(131, 84);
                torso.cubicTo(126, 91, 111, 88, 108, 101);
                torso.cubicTo(108, 116, 120, 130, 122, 146);
                torso.cubicTo(119, 157, 122, 170, 131, 172);
                torso.lineTo(149, 172);
                torso.cubicTo(158, 169, 161, 157, 158, 146);
                torso.cubicTo(160, 130, 172, 116, 172, 101);
                torso.cubicTo(169, 88, 154, 91, 149, 84);
                torso.closeSubPath();
                juce::ColourGradient muscle(skinLight.withAlpha(.96f * flesh), 123, 94,
                    skinShadow.withAlpha(.96f * flesh), 164, 166, false);
                muscle.addColour(.54, skinLight.darker(.24f).withAlpha(.96f * flesh));
                g.setGradientFill(muscle);
                g.fillPath(torso);
                g.setColour(skinShadow.withAlpha(.45f * flesh));
                g.drawLine(140, 100, 140, 142, 1.4f);
                g.setColour(skinLight.brighter(.14f).withAlpha(.31f * flesh));
                g.drawLine(122, 106, 130, 128, 2.0f);
            }
            // A pair of flexible neck connectors follows the head offset
            // while all other vertebrae stay attached to the torso.
            if (flesh > .001f) {
                g.setColour(skinShadow.withAlpha(.90f * flesh));
                g.drawLine(140.0f + head.x, 78.0f + head.y, 140, 92, 12.0f * flesh);
                g.setColour(skinLight.withAlpha(.87f * flesh));
                g.drawLine(140.0f + head.x, 78.0f + head.y, 140, 92, 8.0f * flesh);
            }
            g.setColour(boneColour.withAlpha(.78f * (1.0f - .92f * flesh)));
            g.drawLine(136.0f + head.x, 77.0f + head.y,
                       137.0f, 89.0f, 1.2f);
            g.drawLine(144.0f + head.x, 77.0f + head.y,
                       143.0f, 90.0f, 1.2f);
            // Keep internal bones under the morph instead of growing skin
            // tubes across the surface of the already-covered torso.
            if (flesh < .001f) {
                for (int i = 0; i < 12; ++i)
                    bone(137, 81 + i * 5, 143, 83 + i * 5, 1.2f);
                bone(140, 89, 113, 97);
                bone(140, 89, 167, 97);
            } else {
                g.setColour(boneColour.withAlpha(.85f * (1.0f - flesh)));
                for (int i = 0; i < 12; ++i)
                    g.drawLine(137, 81 + i * 5, 143, 83 + i * 5, 1.2f);
                g.drawLine(140, 89, 113, 97, 2.0f);
                g.drawLine(140, 89, 167, 97, 2.0f);
            }
            // Five smoothly connected ribs per side. The old rib cage
            // formed 70 separately moved line segments each frame, making
            // the chest look like a stack of jagged zigzags even after
            // limiting bone displacement. Continuous paths preserve shape
            // and need only 10 strokes; audio reacts via a gentle offset.
            for (int rib = 0; rib < 5; ++rib) {
                const float y = 99.0f + rib * 9.0f;
                const float reach = 27.0f - 1.2f * std::abs(rib - 2);
                const float breathe = reducedMotion ? 0.0f
                    : activity * .65f * std::sin(static_cast<float>(phase) * .6f + rib * .35f);
                for (const int side : {-1, 1}) {
                    const float d = static_cast<float>(side);
                    juce::Path arch;
                    arch.startNewSubPath(140.0f, y);
                    arch.cubicTo(140 + d * 9, y - 4 + breathe,
                                 140 + d * reach, y - 2 + breathe,
                                 140 + d * reach, y + 3 + breathe);
                    arch.cubicTo(140 + d * reach, y + 8 + breathe,
                                 140 + d * 10, y + 10,
                                 140, y + 8);
                    g.setColour(boneColour.withAlpha((.84f + .12f * activity) * (1.0f - .94f * flesh)));
                    g.strokePath(arch, juce::PathStrokeType(1.35f));
                }
            }
            const float sway = static_cast<float>(std::sin(phase * 1.2) * 6 * movement);
            bone(113, 97, 102, 139 + sway);
            const float wrist = reducedMotion ? 0.f : 13.f * (1.f - activity)
                * std::pow(std::max(0.0, std::sin(phase * .47) * std::cos(phase * .21)), 2.0);
            const float kick = reducedMotion ? 0.f : 15.f * (1.f - activity)
                * std::pow(std::max(0.0, std::cos(phase * .39) * std::sin(phase * .26)), 2.0);
            bone(102, 139 + sway, 97 - wrist, 177 - wrist);
            bone(167, 97, 178, 139 - sway);
            bone(178, 139 - sway, 183, 177);
            for (int i = 0; i < 4; ++i) {
                bone(97 + i * 2 - wrist, 177 - wrist, 91 + i * 3 - wrist, 190 - 1.3f * wrist, 1);
                bone(183 - i * 2, 177, 189 - i * 3, 190, 1);
            }
            if (flesh < .001f) {
                bone(124, 146, 156, 146);
                bone(156, 146, 150, 165);
                bone(150, 165, 140, 159);
                bone(140, 159, 130, 165);
                bone(130, 165, 124, 146);
            } else {
                juce::Path pelvis;
                pelvis.startNewSubPath(124, 146);
                pelvis.lineTo(156, 146); pelvis.lineTo(150, 165);
                pelvis.lineTo(140, 159); pelvis.lineTo(130, 165);
                pelvis.closeSubPath();
                g.setColour(boneColour.withAlpha(.85f * (1.0f - flesh)));
                g.strokePath(pelvis, juce::PathStrokeType(2.0f));
            }
            bone(130, 163, 124, 199);
            bone(124, 199, 119 - kick, 231 - .6f * kick);
            bone(150, 163, 156, 199);
            bone(156, 199, 161, 231);
            if (activity < .2f && flesh < .7f)
            for (auto point : {juce::Point<float>(102, 139 + sway), juce::Point<float>(178, 139 - sway),
                               juce::Point<float>(124, 199), juce::Point<float>(156, 199)})
                g.drawEllipse(point.x - 3, point.y - 3, 6, 6, 1.2f);
            bone(119 - kick, 231 - .6f * kick, 108 - kick, 237 - kick);
            bone(108 - kick, 237 - kick, 125 - kick, 237 - .6f * kick);
            bone(161, 231, 172, 237);
            bone(172, 237, 155, 237);
        }
        // Front-surface reflection above the character.
        {
            juce::Graphics::ScopedSaveState frontGlass(g);
            juce::Path clip;
            clip.addEllipse(18, 18, 244, 244);
            g.reduceClipRegion(clip);
            // The old broad oval looked painted onto the glass. A tapered
            // upper specular crescent and a soft interior glint imply curvature.
            juce::ColourGradient glint(juce::Colours::white.withAlpha(.13f), 66, 35,
                                        juce::Colours::white.withAlpha(0.0f), 126, 108, false);
            g.setGradientFill(glint);
            juce::Path reflection;
            reflection.addEllipse(48, 40, 101, 28);
            reflection.applyTransform(juce::AffineTransform::rotation(-.60f, 100, 56));
            g.fillPath(reflection);
            juce::Path crescent;
            crescent.addCentredArc(140, 140, 112, 112, 0, -1.12f, -.25f, true);
            g.setColour(juce::Colours::white.withAlpha(.30f));
            g.strokePath(crescent, juce::PathStrokeType(2.5f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path lowerRim;
            lowerRim.addCentredArc(140, 140, 118, 118, 0, 2.0f, 3.8f, true);
            g.setColour(accent.withAlpha(.22f + .12f * activity));
            g.strokePath(lowerRim, juce::PathStrokeType(2.0f));
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
        glass.addColour(.19, juce::Colour(0xff285b3c));
        glass.addColour(.40, juce::Colour(0xff163623));
        glass.addColour(.72, juce::Colour(0xff07190f));
        glass.addColour(.93, juce::Colour(0xff020704));
        g.setGradientFill(glass);
        g.fillEllipse(18, 18, 244, 244);
        // Cached refractive layers increase spherical depth at low GPU cost.
        juce::ColourGradient refraction(accent.withAlpha(.12f), 83, 65,
                                         accent.withAlpha(0.0f), 224, 237, true);
        g.setGradientFill(refraction);
        g.fillEllipse(21, 21, 238, 238);
        juce::ColourGradient shadow(juce::Colours::black.withAlpha(0.0f), 139, 142,
                                    juce::Colours::black.withAlpha(.27f), 252, 242, true);
        g.setGradientFill(shadow);
        g.fillEllipse(20, 20, 240, 240);
        juce::ColourGradient occlusion(juce::Colours::black.withAlpha(0.0f), 140, 140,
                                      juce::Colours::black.withAlpha(.72f), 262, 140, true);
        occlusion.addColour(.68, juce::Colours::black.withAlpha(.05f));
        g.setGradientFill(occlusion);
        g.fillEllipse(18, 18, 244, 244);
        juce::ColourGradient bounce(accent.withAlpha(.18f), 160, 228,
                                    accent.withAlpha(0.0f), 160, 158, true);
        g.setGradientFill(bounce);
        g.fillEllipse(92, 158, 136, 94);
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
        g.setColour(juce::Colours::black.withAlpha(.44f));
        g.drawEllipse(19, 19, 242, 242, 2.4f);
        juce::Path reflectiveRim;
        reflectiveRim.addCentredArc(140, 140, 123, 123, 0, -1.75f, -.22f, true);
        g.setColour(juce::Colours::white.withAlpha(.28f));
        g.strokePath(reflectiveRim, juce::PathStrokeType(2.1f));
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
