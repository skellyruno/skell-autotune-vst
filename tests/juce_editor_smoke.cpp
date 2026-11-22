// Real JUCE editor component snapshots (not design mockups), plus an actual
// Slider mapping check. Runs on Windows CI after the full VST3 build.
#include "PluginProcessor.h"
#include "RetuneSpeedSlider.h"
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;

    humtune::RetuneSpeedSlider retune;
    retune.setRange(0.0, 1.0, 0.01);
    const auto near = [](double x, double y) { return std::abs(x - y) < 0.00001; };
    if (!near(retune.valueToProportionOfLength(0.0), 1.0)
        || !near(retune.valueToProportionOfLength(1.0), 0.0)
        || !near(retune.proportionOfLengthToValue(0.0), 1.0)
        || !near(retune.proportionOfLengthToValue(1.0), 0.0)
        || !near(retune.proportionOfLengthToValue(0.25), 0.75))
    {
        std::cerr << "FAIL: clockwise speed direction or inverse mapping\n";
        return 1;
    }

    HumHouseVocalTuneProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    if (!editor)
    {
        std::cerr << "FAIL: OG SkellyTune editor could not be created\n";
        return 2;
    }

    const juce::File outputDir = argc > 1
        ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
        : juce::File::getCurrentWorkingDirectory().getChildFile("reports/ui");
    if (!outputDir.createDirectory() && !outputDir.isDirectory())
    {
        std::cerr << "FAIL: screenshot output directory\n";
        return 3;
    }

    struct Scene { int width; int height; int theme; const char* name; };
    constexpr std::array<Scene, 4> scenes{{
        {420, 408, 0, "small-theme0"},
        {476, 462, 1, "default-theme1"},
        {700, 680, 2, "design-theme2"},
        {1050, 1020, 3, "large-theme3"}
    }};
    for (const auto& scene : scenes)
    {
        processor.setUiTheme(scene.theme);
        editor->setSize(scene.width, scene.height);
        // Process the real UI message loop to apply its theme/timer state.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(110);
        for (int i = 0; i < editor->getNumChildComponents(); ++i)
        {
            const auto* child = editor->getChildComponent(i);
            if (child->isVisible() && !editor->getLocalBounds().contains(child->getBounds()))
            {
                std::cerr << "FAIL: offscreen editor control at "
                          << scene.name << " / " << child->getName() << "\n";
                return 4;
            }
        }
        auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true);
        if (!image.isValid() || image.getWidth() != scene.width
            || image.getHeight() != scene.height)
        {
            std::cerr << "FAIL: invalid JUCE editor screenshot at "
                      << scene.name << "\n";
            return 5;
        }
        const auto file = outputDir.getChildFile(
            "skellytune-" + juce::String(scene.name) + ".png");
        juce::FileOutputStream stream(file);
        juce::PNGImageFormat png;
        if (!stream.openedOk() || !png.writeImageToStream(image, stream))
        {
            std::cerr << "FAIL: PNG write at " << scene.name << "\n";
            return 6;
        }
        stream.flush();
        if (file.getSize() < 1024)
        {
            std::cerr << "FAIL: empty screenshot at " << scene.name << "\n";
            return 7;
        }
        std::cout << "PASS real JUCE editor snapshot: " << file.getFullPathName()
                  << " (" << scene.width << "x" << scene.height << ")\n";
    }
    editor.reset();
    processor.releaseResources();
    std::cout << "PASS retune speed direction: clockwise=fast, APVTS 0=fast\n";
    return 0;
}
