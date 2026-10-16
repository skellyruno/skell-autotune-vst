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
        { "Hard Snap (T-Pain)",    0.00f, 0.00f, 2,    false, true,    -1 },
        { "Sharp Correct",         0.05f, 0.00f, 0,    false, true,    -1 },
        { "Natural Correct",       0.25f, 0.15f, 0,    false, true,    -1 },
        { "Gentle Touch",          0.50f, 0.30f, 0,    false, true,    -1 },
        { "Subtle Polish",         0.70f, 0.40f, 0,    false, true,    -1 },
        { "Robotic",               0.00f, 0.00f, 2,    false, false,   -1 },
        { "Live Performance",      0.15f, 0.10f, 0,    false, true,     2 },
        { "R&B Smooth",            0.10f, 0.05f, 1,    false, true,    -1 },
        { "Pop Vocal",             0.08f, 0.02f, 0,    false, true,    -1 },
        { "Trap Vocal",            0.00f, 0.00f, 1,    false, true,    -1 },
        { "Gospel Sustain",        0.20f, 0.10f, 0,    false, true,    -1 },
        { "Lo-Fi Drift",           0.60f, 0.50f, 2,    false, true,    -1 },
        // Default program: fastest retune, full correction, Note Stabilizer off, formant
        // preservation enabled, and the established quality-first Standard shifter.
        // Preserve the host's voice-range choice for other singers. Low Latency remains an
        // explicit experimental opt-in rather than silently changing the default sound.
        { "Studio Max (Standard)",   0.00f, 0.00f, 0,    false, true,    -1, 0 },
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

    // Flex-Tune is additive and defaults to zero so existing sessions keep their sound.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"flexTune", 1}, "Flex-Tune",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    // Independent target portamento; zero is deliberately neutral for legacy sessions.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"glide", 1}, "Target Glide",
        juce::NormalisableRange<float>(0.0f, 500.0f, 1.0f), 0.0f, "ms"));

    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"rootNote", 1}, "Root Note", 0, 11, 0));

    // Keep the historical 0..2 Scale Type parameter untouched for saved sessions and
    // host automation. New scale families are selected through a separate extension ID.
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"scaleType", 1}, "Scale Type", 0, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"scaleExtension", 1}, "Scale Extension", 0,
        humtune::ScaleQuantizer::kNumScales - 3, 0));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"referenceFreq", 1}, "Reference Frequency",
        juce::NormalisableRange<float>(400.0f, 480.0f, 0.1f), 440.0f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"noteStabilizer", 1}, "Note Stabilizer", false));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"formantPreserve", 1}, "Formant Preserve", true));

    // Independent vocal-tract colour shift. The zero default is intentionally
    // neutral so prior sessions retain the existing sound until this is adjusted.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"formantShift", 1}, "Formant Shift",
        juce::NormalisableRange<float>(-4.0f, 4.0f, 0.1f), 0.0f, "st"));

    // Voice range: the lowest note the tuner follows. Lower notes need more delay.
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"range", 1}, "Voice Range", 0, humtune::AutoTuneEngine::kNumRanges - 1,
        humtune::AutoTuneEngine::kDefaultRange));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"enabled", 1}, "Enabled", true));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"lowLatencyMode", 1}, "Low Latency (Experimental)", false));

    // Hidden from the current editor but retained so older sessions/automation remain valid.
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
    cacheParameterRefs();
    registerPresetTracking(true);
}

HumHouseVocalTuneProcessor::~HumHouseVocalTuneProcessor()
{
    cancelPendingUpdate();
    registerPresetTracking(false);
}

void HumHouseVocalTuneProcessor::parameterChanged(const juce::String&, float)
{
    // Called from either the host's automation thread or a UI thread.
    // An atomic write is safe and does not allocate or block the audio callback.
    currentPreset.store(-1, std::memory_order_relaxed);
}

void HumHouseVocalTuneProcessor::registerPresetTracking(bool add)
{
    static constexpr const char* kParameters[] = {
        "retune", "humanize", "flexTune", "glide", "rootNote", "scaleType",
        "scaleExtension", "referenceFreq", "noteStabilizer", "formantPreserve",
        "formantShift", "range", "enabled", "lowLatencyMode", "inputGain", "outputGain"
    };
    for (const auto* id : kParameters)
    {
        if (add) apvts.addParameterListener(id, this);
        else apvts.removeParameterListener(id, this);
    }
    for (int note = 0; note < 12; ++note)
    {
        const auto id = "note" + juce::String(note);
        if (add) apvts.addParameterListener(id, this);
        else apvts.removeParameterListener(id, this);
    }
}

void HumHouseVocalTuneProcessor::cacheParameterRefs()
{
    parameterRefs.retune = apvts.getRawParameterValue("retune");
    parameterRefs.humanize = apvts.getRawParameterValue("humanize");
    parameterRefs.flexTune = apvts.getRawParameterValue("flexTune");
    parameterRefs.glide = apvts.getRawParameterValue("glide");
    parameterRefs.rootNote = apvts.getRawParameterValue("rootNote");
    parameterRefs.scaleType = apvts.getRawParameterValue("scaleType");
    parameterRefs.scaleExtension = apvts.getRawParameterValue("scaleExtension");
    parameterRefs.referenceFreq = apvts.getRawParameterValue("referenceFreq");
    parameterRefs.noteStabilizer = apvts.getRawParameterValue("noteStabilizer");
    parameterRefs.formantPreserve = apvts.getRawParameterValue("formantPreserve");
    parameterRefs.formantShift = apvts.getRawParameterValue("formantShift");
    parameterRefs.range = apvts.getRawParameterValue("range");
    parameterRefs.enabled = apvts.getRawParameterValue("enabled");
    parameterRefs.lowLatencyMode = apvts.getRawParameterValue("lowLatencyMode");
    parameterRefs.inputGain = apvts.getRawParameterValue("inputGain");
    parameterRefs.outputGain = apvts.getRawParameterValue("outputGain");

    for (int i = 0; i < static_cast<int>(parameterRefs.notes.size()); ++i)
    {
        const auto id = "note" + juce::String(i);
        parameterRefs.notes[static_cast<size_t>(i)] = apvts.getRawParameterValue(id);
    }
}

// ---------------------------------------------------------------------------
// Prepare / Process
// ---------------------------------------------------------------------------
void HumHouseVocalTuneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setVoiceRange(static_cast<int>(parameterRefs.range->load(std::memory_order_relaxed)));
    engine.setLowLatencyMode(parameterRefs.lowLatencyMode->load(std::memory_order_relaxed) > 0.5f);
    engine.prepare(sampleRate, samplesPerBlock);
    reportedLatency = engine.getLatencySamples();
    setLatencySamples(reportedLatency);

}

void HumHouseVocalTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Read parameters
    // Read cached atomics with relaxed ordering: values are independent scalar controls,
    // and no pointer/map/string work is needed inside the audio callback.
    const float retune     = parameterRefs.retune->load(std::memory_order_relaxed); // 1 = fastest
    const float hum        = parameterRefs.humanize->load(std::memory_order_relaxed);
    const float flexTune   = parameterRefs.flexTune->load(std::memory_order_relaxed);
    const float glideMs    = parameterRefs.glide->load(std::memory_order_relaxed);
    const int root         = static_cast<int>(parameterRefs.rootNote->load(std::memory_order_relaxed));
    const int legacyScale  = static_cast<int>(parameterRefs.scaleType->load(std::memory_order_relaxed));
    const int extension    = static_cast<int>(parameterRefs.scaleExtension->load(std::memory_order_relaxed));
    const int scale        = extension > 0 ? extension + 2 : legacyScale;
    const float refFreq    = parameterRefs.referenceFreq->load(std::memory_order_relaxed);
    const bool stab        = parameterRefs.noteStabilizer->load(std::memory_order_relaxed) > 0.5f;
    const bool formant     = parameterRefs.formantPreserve->load(std::memory_order_relaxed) > 0.5f;
    const float formantShift = parameterRefs.formantShift->load(std::memory_order_relaxed);
    const int range        = static_cast<int>(parameterRefs.range->load(std::memory_order_relaxed));
    const bool on          = parameterRefs.enabled->load(std::memory_order_relaxed) > 0.5f;
    const bool lowLatency  = parameterRefs.lowLatencyMode->load(std::memory_order_relaxed) > 0.5f;
    const float inGainDb   = parameterRefs.inputGain->load(std::memory_order_relaxed);
    const float outGainDb  = parameterRefs.outputGain->load(std::memory_order_relaxed);

    // The piano keys are the scale
    std::array<bool, 12> keyMask;
    bool anyKeyOn = false;
    for (int i = 0; i < 12; ++i)
    {
        keyMask[static_cast<size_t>(i)] =
            parameterRefs.notes[static_cast<size_t>(i)]->load(std::memory_order_relaxed) > 0.5f;
        anyKeyOn = anyKeyOn || keyMask[static_cast<size_t>(i)];
    }

    engine.setRootNote(root);
    engine.setScaleType(scale);
    engine.setRetuneSpeed(1.0f - retune);      // the engine works in "0 = instant"
    engine.setHumanize(hum);
    engine.setFlexTune(flexTune);
    engine.setGlideMilliseconds(glideMs);
    engine.setReferenceFreq(refFreq);
    engine.setNoteStabilizer(stab);
    engine.setFormantPreserve(formant);
    engine.setFormantShiftSemitones(formantShift);
    engine.setVoiceRange(range);
    engine.setLowLatencyMode(lowLatency);
    engine.setEnabled(on);

    // The delay depends on the Voice Range: tell the host when it changes
    if (engine.getLatencySamples() != reportedLatency)
    {
        reportedLatency = engine.getLatencySamples();
        pendingLatency.store(reportedLatency);
        triggerAsyncUpdate();
    }

    // If the piano matches the selected named scale, use its full interval definition
    // (including quarter tones). A changed piano mask is an explicit 12-TET custom scale.
    const auto expectedMask = humtune::ScaleQuantizer::approximateMask(scale, root);
    if (anyKeyOn && keyMask != expectedMask)
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

    // Set after broadcasting preset controls so the selector reflects this profile.
    currentPreset.store(-1, std::memory_order_relaxed);
    const auto& p = presets[static_cast<size_t>(index)];

    auto set = [&](const juce::String& id, float val)
    {
        if (auto* param = apvts.getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(val));
    };

    set("retune",          1.0f - p.speed);
    set("humanize",        p.humanize);
    set("scaleType",       static_cast<float>(p.scaleType));
    set("scaleExtension",  0.0f);
    applyScaleToKeys(p.scaleType, static_cast<int>(*apvts.getRawParameterValue("rootNote")));
    set("noteStabilizer",  p.stabilizer ? 1.0f : 0.0f);
    set("formantPreserve", p.formantPreserve ? 1.0f : 0.0f);
    if (p.range >= 0)
        set("range",       static_cast<float>(p.range));
    // Most legacy programs intentionally leave Low Latency alone. Studio Max (Standard)
    // explicitly selects the quality-first Standard path; Low Latency is opt-in.
    if (p.lowLatencyMode >= 0)
        set("lowLatencyMode", p.lowLatencyMode != 0 ? 1.0f : 0.0f);
    // These controls were added after the original factory profiles. A preset
    // must not unexpectedly inherit a prior preset's Glide or Flex-Tune state.
    set("flexTune", 0.0f);
    set("glide", 0.0f);
    set("formantShift", 0.0f);
    currentPreset.store(index, std::memory_order_relaxed);
}

humtune::KeyEstimator::Estimate HumHouseVocalTuneProcessor::applyAutoKeyEstimate()
{
    const auto estimate = engine.getAutoKeyEstimate();
    if (!estimate.valid)
        return estimate;

    auto set = [this](const juce::String& id, float value)
    {
        if (auto* parameter = apvts.getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };

    // Auto-key intentionally selects the familiar 12-TET Major/Minor modes only;
    // microtonal or custom-scale choices remain user-controlled when this button is not used.
    set("rootNote", static_cast<float>(estimate.root));
    set("scaleType", estimate.minor ? 1.0f : 0.0f);
    set("scaleExtension", 0.0f);
    applyScaleToKeys(estimate.minor ? 1 : 0, estimate.root);
    return estimate;
}

void HumHouseVocalTuneProcessor::applyScaleToKeys (int scaleType, int root)
{
    const auto mask = humtune::ScaleQuantizer::approximateMask(scaleType, root);
    for (int i = 0; i < 12; ++i)
        if (auto* param = apvts.getParameter("note" + juce::String(i)))
            param->setValueNotifyingHost(mask[static_cast<size_t>(i)] ? 1.0f : 0.0f);
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
    // Parameter values are host-managed; program identity is editor metadata.
    // Store it outside the parameter list to preserve existing automation IDs.
    state.setProperty("factoryProgramIndex", currentPreset.load(std::memory_order_relaxed), nullptr);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void HumHouseVocalTuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(apvts.state.getType()))
    {
        auto restored = juce::ValueTree::fromXml(*xml);
        // Older projects did not save program identity. Avoid falsely showing
        // Studio Max for an arbitrary restored collection of parameter values.
        const int index = static_cast<int>(restored.getProperty("factoryProgramIndex", -1));
        apvts.replaceState(restored);
        // Restore identity last: parameter listeners may mark the replacement
        // custom while the host applies its saved values.
        currentPreset.store(index >= 0 && index < getNumPrograms() ? index : -1,
                            std::memory_order_relaxed);
    }
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
