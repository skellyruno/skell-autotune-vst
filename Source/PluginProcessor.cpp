#include "PluginProcessor.h"
#include "PluginEditor.h"

// Note names for combo-box choices
static const juce::StringArray kNoteNames {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
const std::vector<HumHouseVocalTuneProcessor::Preset>&
HumHouseVocalTuneProcessor::getPresets()
{
    static const std::vector<Preset> presets =
    {
        // name,                   speed, human, scale, stab,  formant, range
        // (speed here: 0 = instant, 1 = slowest)
        { "Hard Snap (T-Pain)",    0.00f, 0.00f, 2,    true,  true,    -1 },
        { "Sharp Correct",         0.05f, 0.00f, 0,    true,  true,    -1 },
        { "Natural Correct",       0.25f, 0.15f, 0,    true,  true,    -1 },
        { "Gentle Touch",          0.50f, 0.30f, 0,    true,  true,    -1 },
        { "Subtle Polish",         0.70f, 0.40f, 0,    true,  true,    -1 },
        { "Robotic",               0.00f, 0.00f, 2,    false, false,   -1 },
        { "Live Performance",      0.15f, 0.10f, 0,    true,  true,     2 },
        { "R&B Smooth",            0.10f, 0.05f, 1,    true,  true,    -1 },
        { "Pop Vocal",             0.08f, 0.02f, 0,    true,  true,    -1 },
        { "Trap Vocal",            0.00f, 0.00f, 1,    true,  true,    -1 },
        { "Gospel Sustain",        0.20f, 0.10f, 0,    true,  true,    -1 },
        { "Lo-Fi Drift",           0.60f, 0.50f, 2,    false, true,    -1 },
    };
    return presets;
}

// ---------------------------------------------------------------------------
// Parameter layout
// ---------------------------------------------------------------------------
juce::AudioProcessorValueTreeState::ParameterLayout
HumHouseVocalTuneProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Retune Speed: the knob is turned clockwise for FASTER tuning (the number shown goes
    // down to 0 at the far right). 1 = instant (hard tune), 0 = slowest.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"retune", 1}, "Retune Speed",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"humanize", 1}, "Humanize",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"amount", 1}, "Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"rootNote", 1}, "Root Note", 0, 11, 0));

    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"scaleType", 1}, "Scale Type", 0, 6, 0));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"referenceFreq", 1}, "Reference Frequency",
        juce::NormalisableRange<float>(400.0f, 480.0f, 0.1f), 440.0f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"noteStabilizer", 1}, "Note Stabilizer", true));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"formantPreserve", 1}, "Formant Preserve", true));

    // Voice range: the lowest note the tuner follows. Lower notes require more shifter delay.
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"range", 1}, "Voice Range", 0, humtune::AutoTuneEngine::kNumRanges - 1,
        humtune::AutoTuneEngine::kDefaultRange));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"enabled", 1}, "Enabled", true));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"inputGain", 1}, "Input Gain",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"outputGain", 1}, "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f));

    // Piano keys: which notes the tuner may snap to. They start as C major, which matches
    // the default Scale (Major) and Key (C). The Scale and Key controls re-light them.
    for (int i = 0; i < 12; ++i)
    {
        auto id = "note" + juce::String(i);
        auto name = kNoteNames[i] + " On";
        const bool inCMajor = isNoteInScale(0, 0, i);
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{id, 1}, name, inCMajor));
    }

    return { params.begin(), params.end() };
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
HumHouseVocalTuneProcessor::HumHouseVocalTuneProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

// ---------------------------------------------------------------------------
// Prepare / Process
// ---------------------------------------------------------------------------
void HumHouseVocalTuneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setVoiceRange(static_cast<int>(*apvts.getRawParameterValue("range")));
    engine.prepare(sampleRate, samplesPerBlock);
    reportedLatency = engine.getLatencySamples();
    setLatencySamples(reportedLatency);

}

void HumHouseVocalTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Read parameters
    float retune    = *apvts.getRawParameterValue("retune");   // 1 = fastest
    float hum       = *apvts.getRawParameterValue("humanize");
    float amount    = *apvts.getRawParameterValue("amount");
    int   root      = static_cast<int>(*apvts.getRawParameterValue("rootNote"));
    int   scale     = static_cast<int>(*apvts.getRawParameterValue("scaleType"));
    float refFreq   = *apvts.getRawParameterValue("referenceFreq");
    bool  stab      = *apvts.getRawParameterValue("noteStabilizer") > 0.5f;
    bool  formant   = *apvts.getRawParameterValue("formantPreserve") > 0.5f;
    int   range     = static_cast<int>(*apvts.getRawParameterValue("range"));
    bool  on        = *apvts.getRawParameterValue("enabled") > 0.5f;
    float inGainDb  = *apvts.getRawParameterValue("inputGain");
    float outGainDb = *apvts.getRawParameterValue("outputGain");

    // The piano keys are the scale
    std::array<bool, 12> keyMask;
    bool anyKeyOn = false;
    for (int i = 0; i < 12; ++i)
    {
        keyMask[static_cast<size_t>(i)] =
            *apvts.getRawParameterValue("note" + juce::String(i)) > 0.5f;
        anyKeyOn = anyKeyOn || keyMask[static_cast<size_t>(i)];
    }

    engine.setRootNote(root);
    engine.setScaleType(scale);
    engine.setRetuneSpeed(1.0f - retune);      // the engine works in "0 = instant"
    engine.setHumanize(hum);
    engine.setAmount(amount);
    engine.setReferenceFreq(refFreq);
    engine.setNoteStabilizer(stab);
    engine.setFormantPreserve(formant);
    engine.setVoiceRange(range);
    engine.setEnabled(on);

    // The delay depends on the Voice Range: tell the host when it changes
    if (engine.getLatencySamples() != reportedLatency)
    {
        reportedLatency = engine.getLatencySamples();
        pendingLatency.store(reportedLatency);
        triggerAsyncUpdate();
    }

    // With every key switched off there is nothing to snap to: fall back to Scale + Key
    if (anyKeyOn)
        engine.setCustomScale(keyMask);
    else
        engine.clearCustomScale();

    // Input gain
    float inGain = juce::Decibels::decibelsToGain(inGainDb);
    buffer.applyGain(inGain);

    // Measure input level
    {
        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            peak = std::max(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
        inputLevel.store(peak);
    }

    // Process autotune
    engine.process(buffer);

    // Output gain
    float outGain = juce::Decibels::decibelsToGain(outGainDb);
    buffer.applyGain(outGain);

    // Measure output level
    {
        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            peak = std::max(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
        outputLevel.store(peak);
    }
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
int HumHouseVocalTuneProcessor::getNumPrograms()
{
    return static_cast<int>(getPresets().size());
}

void HumHouseVocalTuneProcessor::setCurrentProgram (int index)
{
    const auto& presets = getPresets();
    if (index < 0 || index >= static_cast<int>(presets.size()))
        return;

    currentPreset = index;
    const auto& p = presets[static_cast<size_t>(index)];

    auto set = [&](const juce::String& id, float val)
    {
        if (auto* param = apvts.getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(val));
    };

    set("retune",          1.0f - p.speed);
    set("humanize",        p.humanize);
    set("scaleType",       static_cast<float>(p.scaleType));
    applyScaleToKeys(p.scaleType, static_cast<int>(*apvts.getRawParameterValue("rootNote")));
    set("noteStabilizer",  p.stabilizer ? 1.0f : 0.0f);
    set("formantPreserve", p.formantPreserve ? 1.0f : 0.0f);
    if (p.range >= 0)
        set("range",       static_cast<float>(p.range));
}

void HumHouseVocalTuneProcessor::applyScaleToKeys (int scaleType, int root)
{
    for (int i = 0; i < 12; ++i)
        if (auto* param = apvts.getParameter("note" + juce::String(i)))
            param->setValueNotifyingHost(isNoteInScale(scaleType, root, i) ? 1.0f : 0.0f);
}

const juce::String HumHouseVocalTuneProcessor::getProgramName (int index)
{
    const auto& presets = getPresets();
    if (index < 0 || index >= static_cast<int>(presets.size()))
        return {};
    return presets[static_cast<size_t>(index)].name;
}

// ---------------------------------------------------------------------------
// State save / restore
// ---------------------------------------------------------------------------
void HumHouseVocalTuneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void HumHouseVocalTuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

// ---------------------------------------------------------------------------
// Editor factory
// ---------------------------------------------------------------------------
juce::AudioProcessorEditor* HumHouseVocalTuneProcessor::createEditor()
{
    return new HumHouseVocalTuneEditor(*this);
}

// ---------------------------------------------------------------------------
// Plugin instantiation
// ---------------------------------------------------------------------------
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HumHouseVocalTuneProcessor();
}
