#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Minimal offline SDK host. It never installs the plugin or controls another
// application's UI. A message pump services JUCE's deferred latency notification.
struct Stream final : IBStream
{
    std::vector<char> bytes;
    int64 position = 0;
    tresult PLUGIN_API queryInterface(const TUID id, void** out) override
    {
        *out = nullptr;
        if (FUnknownPrivate::iidEqual(id, IBStream_iid)) { *out = this; return kResultOk; }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API read(void* dst, int32 n, int32* done) override
    {
        if (n < 0 || position < 0 || position > static_cast<int64>(bytes.size())) return kInvalidArgument;
        const int32 count = static_cast<int32>(std::min<int64>(n, bytes.size() - position));
        std::memcpy(dst, bytes.data() + position, count); position += count;
        if (done) *done = count;
        return count > 0 || n == 0 ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API write(void* src, int32 n, int32* done) override
    {
        if (n < 0 || position < 0) return kInvalidArgument;
        bytes.resize(static_cast<size_t>(std::max<int64>(bytes.size(), position + n)));
        std::memcpy(bytes.data() + position, src, n); position += n;
        if (done) *done = n;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset, int32 mode, int64* result) override
    {
        position = offset + (mode == kIBSeekCur ? position : mode == kIBSeekEnd ? bytes.size() : 0);
        if (result) *result = position;
        return position >= 0 ? kResultOk : kInvalidArgument;
    }
    tresult PLUGIN_API tell(int64* result) override { if (!result) return kInvalidArgument; *result = position; return kResultOk; }
};
struct Queue final : IParamValueQueue
{
    ParamID id = 0; ParamValue value = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** out) override { *out = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 i, int32& offset, ParamValue& result) override
    { if (i != 0) return kInvalidArgument; offset = 0; result = value; return kResultOk; }
    tresult PLUGIN_API addPoint(int32, ParamValue v, int32& i) override { value = v; i = 0; return kResultOk; }
};
struct Changes final : IParameterChanges
{
    Queue queue;
    tresult PLUGIN_API queryInterface(const TUID, void** out) override { *out = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 i) override { return i == 0 ? &queue : nullptr; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& i) override { queue.id = id; i = 0; return &queue; }
};
static void pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
}
static std::string narrow(const TChar* text)
{
    std::string result;
    for (; *text; ++text) result.push_back(static_cast<char>(*text));
    return result;
}
int main(int argc, char** argv)
{
    const bool renderMode = argc == 10 || argc == 11;
    if (argc != 3 && argc != 4 && !renderMode)
    {
        std::cerr << "usage: host plugin.vst3 output-prefix [legacy-state.bin]\n"
                     "or: host plugin.vst3 prefix input.raw output.wav frames root low range protection [formants]\n";
        return 2;
    }
    const auto module = LoadLibraryA(argv[1]); assert(module);
    const auto init = reinterpret_cast<bool(*)()>(GetProcAddress(module, "InitDll"));
    const auto exit = reinterpret_cast<bool(*)()>(GetProcAddress(module, "ExitDll"));
    const auto factoryFn = reinterpret_cast<IPluginFactory*(*)()>(GetProcAddress(module, "GetPluginFactory"));
    assert(init && exit && factoryFn && init());
    auto* factory = factoryFn(); assert(factory);
    TUID classId{};
    for (int i = 0; i < factory->countClasses(); ++i)
    {
        PClassInfo info{}; assert(factory->getClassInfo(i, &info) == kResultOk);
        if (std::string(info.category) == kVstAudioEffectClass) std::memcpy(classId, info.cid, sizeof(TUID));
    }
    std::ofstream manifest(std::string(argv[2]) + "-parameters.csv");
    manifest << "title,id,steps,default\n";
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        IComponent* component = nullptr;
        assert(factory->createInstance(classId, IComponent_iid, reinterpret_cast<void**>(&component)) == kResultOk);
        assert(component->initialize(nullptr) == kResultOk);
        IAudioProcessor* processor = nullptr;
        assert(component->queryInterface(IAudioProcessor_iid, reinterpret_cast<void**>(&processor)) == kResultOk);
        TUID controllerId{}; assert(component->getControllerClassId(controllerId) == kResultOk);
        IEditController* controller = nullptr;
        assert(factory->createInstance(controllerId, IEditController_iid, reinterpret_cast<void**>(&controller)) == kResultOk);
        assert(controller->initialize(nullptr) == kResultOk);
        IConnectionPoint* componentConnection = nullptr;
        IConnectionPoint* controllerConnection = nullptr;
        assert(component->queryInterface(IConnectionPoint_iid, reinterpret_cast<void**>(&componentConnection)) == kResultOk);
        assert(controller->queryInterface(IConnectionPoint_iid, reinterpret_cast<void**>(&controllerConnection)) == kResultOk);
        assert(componentConnection->connect(controllerConnection) == kResultOk);
        assert(controllerConnection->connect(componentConnection) == kResultOk);
        if (argc == 4)
        {
            std::ifstream file(argv[3], std::ios::binary);
            assert(file.good());
            Stream legacy;
            legacy.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            assert(component->setState(&legacy) == kResultOk);
        }
        Stream initial; assert(component->getState(&initial) == kResultOk && initial.bytes.size() > 100);
        initial.position = 0;
        assert(controller->setComponentState(&initial) == kResultOk);
        std::map<std::string, ParamID> ids;
        for (int i = 0; i < controller->getParameterCount(); ++i)
        {
            ParameterInfo info{}; assert(controller->getParameterInfo(i, info) == kResultOk);
            const auto name = narrow(info.title); ids[name] = info.id;
            if (rate == 44100.0) manifest << name << ',' << info.id << ',' << info.stepCount << ',' << info.defaultNormalizedValue << '\n';
        }
        manifest.flush();
        assert(ids.count("Enabled") && ids.count("Low Latency (Experimental)"));
        SpeakerArrangement stereo = SpeakerArr::kStereo;
        assert(processor->setBusArrangements(&stereo, 1, &stereo, 1) == kResultOk);
        assert(component->activateBus(kAudio, kInput, 0, true) == kResultOk);
        assert(component->activateBus(kAudio, kOutput, 0, true) == kResultOk);
        ProcessSetup setup{}; setup.processMode = kRealtime; setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 512; setup.sampleRate = rate;
        assert(processor->setupProcessing(setup) == kResultOk);
        assert(component->setActive(true) == kResultOk);
        assert(processor->setProcessing(true) == kResultOk);
        // Stored profiles can intentionally restrict the detector floor. Derive
        // PDC from recalled controls rather than assuming Catch Low Notes is on.
        const int initialRange = std::clamp(static_cast<int>(std::lround(
            controller->getParamNormalized(ids.at("Voice Range")) * 5.0)), 0, 5);
        constexpr double profileFloors[] = {70.0, 80.0, 100.0, 130.0, 180.0, 40.0};
        const bool catchLow = controller->getParamNormalized(ids.at("Catch Low Notes")) > 0.5;
        const double initialFloor = catchLow ? std::min(profileFloors[initialRange], 80.0)
                                           : profileFloors[initialRange];
        const int standardDelay = static_cast<int>(std::ceil(1.3 * std::ceil(rate / initialFloor))) + 8;
        const bool initialLow = controller->getParamNormalized(ids.at("Low Latency (Experimental)")) > 0.5;
        assert(processor->getLatencySamples() == static_cast<uint32>(initialLow ? std::ceil(rate * .0019) : standardDelay));
        std::vector<float> left(512), right(512), outLeft(512), outRight(512);
        float* inPointers[] = {left.data(), right.data()}, *outPointers[] = {outLeft.data(), outRight.data()};
        AudioBusBuffers input{}, output{}; input.numChannels = output.numChannels = 2;
        input.channelBuffers32 = inPointers; output.channelBuffers32 = outPointers;
        ProcessData data{}; data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
        data.numSamples = 127; data.numInputs = data.numOutputs = 1; data.inputs = &input; data.outputs = &output;
        const auto set = [&](const char* name, double value) {
            Changes changes; changes.queue.id = ids.at(name); changes.queue.value = value;
            data.inputParameterChanges = &changes;
            assert(processor->process(data) == kResultOk);
            data.inputParameterChanges = nullptr; pump();
        };
        const auto cleanup = [&] {
            componentConnection->disconnect(controllerConnection);
            controllerConnection->disconnect(componentConnection);
            componentConnection->release(); controllerConnection->release();
            controller->terminate(); controller->release(); processor->release();
            component->terminate(); component->release(); pump();
        };
        if (renderMode)
        {
            set("Retune Speed", 1); set("Formant Preserve", argc == 11 ? std::stoi(argv[10]) : 0); set("Enabled", 1);
            set("Root Note", std::stoi(argv[6]) / 11.0);
            set("Low Latency (Experimental)", std::stoi(argv[7]));
            set("Voice Range", std::stoi(argv[8]) / 5.0);
            set("Catch Low Notes", std::stoi(argv[9]));
            assert(processor->setProcessing(false) == kResultOk);
            assert(component->setActive(false) == kResultOk);
            assert(component->setActive(true) == kResultOk);
            assert(processor->setProcessing(true) == kResultOk);
            const int count = std::stoi(argv[5]); assert(count > 0);
            std::vector<float> source(static_cast<size_t>(count)*2), result(source.size());
            std::ifstream file(argv[3], std::ios::binary);
            file.read(reinterpret_cast<char*>(source.data()), source.size()*sizeof(float));
            assert(file.gcount() == static_cast<std::streamsize>(source.size()*sizeof(float)));
            for (int offset = 0; offset < count;)
            {
                data.numSamples = std::min(127, count-offset);
                for (int i=0; i<data.numSamples; ++i)
                { left[i]=source[static_cast<size_t>(offset+i)*2]; right[i]=source[static_cast<size_t>(offset+i)*2+1]; }
                assert(processor->process(data) == kResultOk);
                for (int i=0; i<data.numSamples; ++i)
                { result[static_cast<size_t>(offset+i)*2]=outLeft[i]; result[static_cast<size_t>(offset+i)*2+1]=outRight[i]; }
                offset += data.numSamples;
            }
            std::ofstream wav(argv[4], std::ios::binary);
            const auto u16=[&](uint16_t v) { for(int i=0;i<2;++i) wav.put(static_cast<char>(v>>(8*i))); };
            const auto u32=[&](uint32_t v) { for(int i=0;i<4;++i) wav.put(static_cast<char>(v>>(8*i))); };
            const auto bytes=static_cast<uint32_t>(result.size()*sizeof(float));
            wav.write("RIFF",4); u32(36+bytes); wav.write("WAVEfmt ",8); u32(16); u16(3); u16(2);
            u32(44100); u32(44100*8); u16(8); u16(32); wav.write("data",4); u32(bytes);
            wav.write(reinterpret_cast<const char*>(result.data()), bytes);
            assert(wav.good());
            std::cout << "Real VST3 vocal rendered frames=" << count << " latency=" << processor->getLatencySamples() << std::endl;
            assert(processor->setProcessing(false) == kResultOk);
            assert(component->setActive(false) == kResultOk);
            cleanup();
            break;
        }
        // Exact dry impulse delay checks the complete wrapper/parameter/DSP path.
        set("Enabled", 0);
        for (int low : {0, 1})
        {
            set("Low Latency (Experimental)", low);
            const int latency = low ? static_cast<int>(std::ceil(rate * .0019)) : standardDelay;
            assert(processor->getLatencySamples() == static_cast<uint32>(latency));
            for (int i = 0; i < 16; ++i) assert(processor->process(data) == kResultOk);
            double largest = 0; int peakPosition = -1;
            // Include the complete recalled delay (Sub Bass at 96 kHz exceeds 2048 samples).
            const int total = std::max(4 * 512, latency + 512);
            for (int offset = 0; offset < total;)
            {
                data.numSamples = std::min(127, total - offset);
                std::fill(left.begin(), left.end(), 0); std::fill(right.begin(), right.end(), 0);
                if (offset == 0) left[0] = right[0] = .5f;
                assert(processor->process(data) == kResultOk);
                for (int i = 0; i < data.numSamples; ++i)
                    if (std::abs(outLeft[i]) > largest) { largest = std::abs(outLeft[i]); peakPosition = offset + i; }
                offset += data.numSamples;
            }
            assert(peakPosition == latency && std::abs(largest - .5) < 1e-6);
            std::cout << "dry-PDC rate=" << rate << " low=" << low << " reported=" << latency << " measured=" << peakPosition << std::endl;
        }
        set("Enabled", 1);
        double energy = 0;
        for (int b = 0; b < 80; ++b)
        {
            data.numSamples = (b % 3 == 0) ? 1 : (b % 3 == 1) ? 127 : 512;
            for (int i = 0; i < data.numSamples; ++i)
                left[i] = right[i] = static_cast<float>(.2*std::sin(6.283185307179586*207*(b*512+i)/rate));
            assert(processor->process(data) == kResultOk);
            for (int i = 0; i < data.numSamples; ++i)
            { assert(std::isfinite(outLeft[i]) && std::isfinite(outRight[i])); energy += outLeft[i]*outLeft[i] + outRight[i]*outRight[i]; }
        }
        assert(energy > 1);
        if (rate == 48000.0)
        {
            set("Low Latency (Experimental)", 0);
            set("Formant Preserve", 0);
            set("Retune Speed", 1);
            set("Root Note", 6.0/11.0); // F# Major contains C#, unlike the old C-major piano.
            const auto pitchProbe = [&](double target) {
                std::vector<float> rendered;
                constexpr int blocks = 90;
                for (int b = 0; b < blocks; ++b)
                {
                    data.numSamples = 512;
                    for (int i = 0; i < 512; ++i)
                        left[i] = right[i] = static_cast<float>(.2*std::sin(6.283185307179586*270*(b*512+i)/rate));
                    assert(processor->process(data) == kResultOk);
                    if (b > 45) rendered.insert(rendered.end(), outLeft.begin(), outLeft.end());
                }
                double best = -1, frequency = 0;
                for (double hz = target-20; hz < target+20; hz += .2)
                {
                    double re=0, im=0;
                    for (size_t i=0; i<rendered.size(); ++i)
                    {
                        const double phase=6.283185307179586*hz*i/rate;
                        re+=rendered[i]*std::cos(phase); im+=rendered[i]*std::sin(phase);
                    }
                    const double magnitude=re*re+im*im;
                    if(magnitude>best) { best=magnitude;frequency=hz; }
                }
                std::cout << "editor-closed target=" << target << " rendered=" << frequency << std::endl;
                assert(std::abs(frequency-target) < 1);
            };
            pitchProbe(277.18263);
            const char* notes[]={"C On","C# On","D On","D# On","E On","F On","F# On","G On","G# On","A On","A# On","B On"};
            for (int i=0;i<12;++i) set(notes[i], i==9 ? 1 : 0);
            pitchProbe(220); // A-only manual mask still overrides the named scale.
            Stream custom; assert(component->getState(&custom) == kResultOk);
            set("Root Note", 0);
            pitchProbe(261.62557); // A named-key change releases the manual mask.
            custom.position = 0;
            assert(component->setState(&custom) == kResultOk);
            pitchProbe(220); // A saved manual mask retains ownership after recall.
        }
        set("Root Note", 7.0/11.0);
        set("Formant Preserve", 0);
        if (rate == 44100.0)
        {
            Stream saved; assert(component->getState(&saved) == kResultOk);
            std::ofstream file(std::string(argv[2]) + "-state.bin", std::ios::binary);
            file.write(saved.bytes.data(), saved.bytes.size());
        }
        assert(processor->setProcessing(false) == kResultOk);
        assert(component->setActive(false) == kResultOk);
        initial.position = 0;
        assert(component->setState(&initial) == kResultOk);
        Stream restored; assert(component->getState(&restored) == kResultOk);
        assert(restored.bytes == initial.bytes);
        std::cout << "state-roundtrip rate=" << rate << " bytes=" << initial.bytes.size() << " pass" << std::endl;
        cleanup();
    }
    factory->release(); assert(exit()); FreeLibrary(module);
}
