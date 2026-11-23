// Actual JUCE editor snapshots, plus real-control/appearance checks.
// This runs on the Windows CI machine after building the VST3.
#include "PluginProcessor.h"
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;
    HumHouseVocalTuneProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    if (!editor)
    {
        std::cerr << "FAIL: OG SkellyTune editor could not be created\n";
        return 1;
    }

    // Inspect the REAL plugin's retune control, not a synthetic spare slider.
    // Approved mapping: clockwise = faster, host retune 1 = instant,
    // UI readout 0 = instant / 100 = slow, DSP engine uses 1 - retune.
    juce::Slider* speed = nullptr;
    juce::Button* cycleTheme = nullptr;
    juce::Label* speedNumber = nullptr;
    juce::Label* humanizeNumber = nullptr;
    std::array<juce::Button*, 12> piano{};
    for (int i = 0; i < editor->getNumChildComponents(); ++i)
    {
        auto* child = editor->getChildComponent(i);
        if (child->getName() == "RETUNE SPEED")
            speed = dynamic_cast<juce::Slider*>(child);
        if (child->getName() == "Cycle theme")
            cycleTheme = dynamic_cast<juce::Button*>(child);
        if (child->getName() == "Retune number")
            speedNumber = dynamic_cast<juce::Label*>(child);
        if (child->getName() == "Humanize number")
            humanizeNumber = dynamic_cast<juce::Label*>(child);
        static const juce::StringArray names{"C", "C#", "D", "D#", "E", "F",
                                              "F#", "G", "G#", "A", "A#", "B"};
        for (int key = 0; key < 12; ++key)
            if (child->getName() == "Piano key " + names[key])
                piano[static_cast<size_t>(key)] = dynamic_cast<juce::Button*>(child);
    }
    auto* storedRetune = processor.getAPVTS().getParameter("retune");
    const auto near = [](double x, double y) { return std::abs(x - y) < 0.00001; };
    if (!speed || !storedRetune
        || !near(storedRetune->getDefaultValue(), 1.0)
        || !near(speed->getMinimum(), 0.0) || !near(speed->getMaximum(), 1.0)
        || !near(speed->valueToProportionOfLength(0.0), 0.0)
        || !near(speed->valueToProportionOfLength(1.0), 1.0)
        || !near(speed->proportionOfLengthToValue(0.0), 0.0)
        || !near(speed->proportionOfLengthToValue(1.0), 1.0))
    {
        std::cerr << "FAIL: original OG SkellyTune retune control changed\n";
        return 2;
    }
    if (!cycleTheme || !cycleTheme->onClick || !speedNumber || !humanizeNumber)
    {
        std::cerr << "FAIL: actual theme-cycle button callback missing\n";
        return 3;
    }

    for (const auto* note : piano)
        if (!note)
        {
            std::cerr << "FAIL: missing one of the 12 piano notes\n";
            return 15;
        }
    speed->setValue(1.0, juce::sendNotificationSync);
    if (speedNumber->getText() != "0")
    {
        std::cerr << "FAIL: retune=1 should read 0 (instant)\n";
        return 16;
    }
    speed->setValue(0.0, juce::sendNotificationSync);
    if (speedNumber->getText() != "100")
    {
        std::cerr << "FAIL: retune=0 should read 100 (slow)\n";
        return 17;
    }
    speed->setValue(1.0, juce::sendNotificationSync);

    const juce::File outputDir = argc > 1
        ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
        : juce::File::getCurrentWorkingDirectory().getChildFile("reports/ui");
    if (!outputDir.createDirectory() && !outputDir.isDirectory())
    {
        std::cerr << "FAIL: screenshot output directory\n";
        return 4;
    }

    struct Scene { int width; int height; int theme; const char* name; };
    constexpr std::array<Scene, 4> scenes{{
        {420, 408, 0, "small-theme0"},
        {476, 462, 1, "default-theme1"},
        {700, 680, 2, "design-theme2"},
        {1050, 1020, 3, "large-theme3"}
    }};
    std::array<juce::Colour, 4> sampledBackgrounds{};
    for (const auto& scene : scenes)
    {
        // The editor timer deliberately does not fire while hidden in CI.
        // Changing only processor.setUiTheme() produced four misleading
        // filenames showing the SAME visual theme. Call the actual button
        // callback, which synchronously applies LookAndFeel + repaints.
        for (int attempts = 0;
             attempts < 4 && processor.getUiTheme() != scene.theme;
             ++attempts)
            cycleTheme->onClick();
        if (processor.getUiTheme() != scene.theme)
        {
            std::cerr << "FAIL: actual theme button cannot select "
                      << scene.name << "\n";
            return 5;
        }
        editor->setSize(scene.width, scene.height);
        if (!piano[0] || !piano[1] || piano[0]->getHeight() <= 0
            || piano[1]->getHeight() <= 0
            || piano[0]->getHeight() >= scene.height / 5
            || piano[1]->getHeight() >= piano[0]->getHeight()
            || speedNumber->getHeight() <= speed->getHeight() / 6)
        {
            std::cerr << "FAIL: compact keyboard/readout geometry at "
                      << scene.name << "\n";
            return 14;
        }

        for (int i = 0; i < editor->getNumChildComponents(); ++i)
        {
            const auto* child = editor->getChildComponent(i);
            if (child->isVisible() && !editor->getLocalBounds().contains(child->getBounds()))
            {
                std::cerr << "FAIL: offscreen editor control at "
                          << scene.name << " / " << child->getName() << "\n";
                return 6;
            }
        }
        const auto image = editor->createComponentSnapshot(
            editor->getLocalBounds(), true);
        if (!image.isValid() || image.getWidth() != scene.width
            || image.getHeight() != scene.height)
        {
            std::cerr << "FAIL: invalid native JUCE editor screenshot at "
                      << scene.name << "\n";
            return 7;
        }

        // Normalized location outside the central panels; different
        // LookAndFeel themes must actually paint DIFFERENT body colours.
        const int x = juce::jlimit(0, scene.width - 1,
                                  juce::roundToInt(scene.width * 10.0 / 700.0));
        const int y = juce::jlimit(0, scene.height - 1,
                                  juce::roundToInt(scene.height * 225.0 / 680.0));
        const auto sample = image.getPixelAt(x, y);
        for (int earlier = 0; earlier < scene.theme; ++earlier)
            if (sample.getARGB() == sampledBackgrounds[static_cast<size_t>(earlier)].getARGB())
            {
                std::cerr << "FAIL: scene theme " << scene.theme
                          << " painted the same background as theme "
                          << earlier << "\n";
                return 8;
            }
        sampledBackgrounds[static_cast<size_t>(scene.theme)] = sample;

        const auto file = outputDir.getChildFile(
            "skellytune-" + juce::String(scene.name) + ".png");
        juce::FileOutputStream stream(file);
        juce::PNGImageFormat png;
        if (!stream.openedOk() || !png.writeImageToStream(image, stream))
        {
            std::cerr << "FAIL: screenshot PNG write at " << scene.name << "\n";
            return 9;
        }
        stream.flush();
        if (file.getSize() < 1024)
        {
            std::cerr << "FAIL: empty screenshot at " << scene.name << "\n";
            return 10;
        }
        std::cout << "PASS genuine JUCE editor: " << scene.name
                  << " (" << scene.width << "x" << scene.height
                  << ", distinct theme body=0x" << std::hex
                  << sample.getARGB() << std::dec << ")\n";
    }
    editor.reset();
    processor.releaseResources();
    std::cout << "PASS approved retune direction: clockwise faster; "
                 "display 0=instant, 100=slow\n";
    return 0;
}
