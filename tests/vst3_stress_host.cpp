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
#include <thread>
#include <atomic>
#include "pluginterfaces/gui/iplugview.h"

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
    static const bool pacedPreview = GetEnvironmentVariableA("SKELLYTUNE_PREVIEW_UI", nullptr, 0) != 0;
    int dispatched = 0;
    while ((!pacedPreview || dispatched++ < 4) && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
}
static std::string narrow(const TChar* text)
{
    std::string result;
    for (; *text; ++text) result.push_back(static_cast<char>(*text));
    return result;
}
// Append to the shared minimal SDK-host declarations from vst3_contract_host.cpp.
#include <chrono>
#include <cstdio>
#include <realtimeapiset.h>
#ifdef NDEBUG
#error "Native process and API checks must remain active"
#endif
int main(int argc,char** argv)
{
    const bool measureStartup=argc>1 && std::string(argv[argc-1])=="--measure-startup";
    const int argumentCount=argc-(measureStartup ? 1 : 0);
    if(argumentCount!=10 && argumentCount!=13) {
        std::fprintf(stderr,"usage: host plugin.vst3 prefix rate block low formants range seconds motion [stereo.raw root catchLowNotes] [--measure-startup]\n");
        return 2;
    }
    const int rate=std::stoi(argv[3]),block=std::stoi(argv[4]),voice=std::stoi(argv[7]);
    const bool low=std::stoi(argv[5])!=0,formants=std::stoi(argv[6])!=0,motion=std::stoi(argv[9])!=0;
    const bool vocal=argumentCount==13;
    const int root=vocal ? std::stoi(argv[11]) : 0;
    const bool protect=vocal && std::stoi(argv[12])!=0;
    const double seconds=std::stod(argv[8]);
    if(rate<8000 || rate>192000 || block<1 || block>512 || voice<0 || voice>5 || seconds<1 || seconds>60 || root<0 || root>11)return 2;
    const auto module=LoadLibraryA(argv[1]);assert(module);
    const auto init=reinterpret_cast<bool(*)()>(GetProcAddress(module,"InitDll"));
    const auto finish=reinterpret_cast<bool(*)()>(GetProcAddress(module,"ExitDll"));
    const auto getFactory=reinterpret_cast<IPluginFactory*(*)()>(GetProcAddress(module,"GetPluginFactory"));
    assert(init && finish && getFactory && init());auto* factory=getFactory();assert(factory);
    TUID classId{};
    for(int i=0;i<factory->countClasses();++i) {PClassInfo info{};assert(factory->getClassInfo(i,&info)==kResultOk);if(std::string(info.category)==kVstAudioEffectClass)std::memcpy(classId,info.cid,sizeof(TUID));}
    IComponent* component=nullptr;IAudioProcessor* processor=nullptr;IEditController* controller=nullptr;
    assert(factory->createInstance(classId,IComponent_iid,reinterpret_cast<void**>(&component))==kResultOk);
    assert(component->initialize(nullptr)==kResultOk);
    assert(component->queryInterface(IAudioProcessor_iid,reinterpret_cast<void**>(&processor))==kResultOk);
    TUID controllerId{};assert(component->getControllerClassId(controllerId)==kResultOk);
    assert(factory->createInstance(controllerId,IEditController_iid,reinterpret_cast<void**>(&controller))==kResultOk);
    assert(controller->initialize(nullptr)==kResultOk);
    IConnectionPoint *componentConnection=nullptr,*controllerConnection=nullptr;
    assert(component->queryInterface(IConnectionPoint_iid,reinterpret_cast<void**>(&componentConnection))==kResultOk);
    assert(controller->queryInterface(IConnectionPoint_iid,reinterpret_cast<void**>(&controllerConnection))==kResultOk);
    assert(componentConnection->connect(controllerConnection)==kResultOk);
    assert(controllerConnection->connect(componentConnection)==kResultOk);
    Stream initial;assert(component->getState(&initial)==kResultOk);initial.position=0;assert(controller->setComponentState(&initial)==kResultOk);
    std::map<std::string,ParamID> ids;
    for(int i=0;i<controller->getParameterCount();++i) {ParameterInfo info{};assert(controller->getParameterInfo(i,info)==kResultOk);ids[narrow(info.title)]=info.id;}
    SpeakerArrangement stereo=SpeakerArr::kStereo;assert(processor->setBusArrangements(&stereo,1,&stereo,1)==kResultOk);
    assert(component->activateBus(kAudio,kInput,0,true)==kResultOk);assert(component->activateBus(kAudio,kOutput,0,true)==kResultOk);
    ProcessSetup setup{};setup.processMode=kRealtime;setup.symbolicSampleSize=kSample32;setup.maxSamplesPerBlock=512;setup.sampleRate=rate;
    assert(processor->setupProcessing(setup)==kResultOk);assert(component->setActive(true)==kResultOk);assert(processor->setProcessing(true)==kResultOk);
    std::vector<float> left(512),right(512),outLeft(512),outRight(512);
    float* inputs[]={left.data(),right.data()};float* outputs[]={outLeft.data(),outRight.data()};
    AudioBusBuffers input{},output{};input.numChannels=output.numChannels=2;input.channelBuffers32=inputs;output.channelBuffers32=outputs;
    ProcessData data{};data.processMode=kRealtime;data.symbolicSampleSize=kSample32;data.numSamples=127;data.numInputs=data.numOutputs=1;data.inputs=&input;data.outputs=&output;
    const auto set=[&](const std::string& name,double value) {Changes change;change.queue.id=ids.at(name);change.queue.value=value;data.inputParameterChanges=&change;assert(processor->process(data)==kResultOk);data.inputParameterChanges=nullptr;pump();};
    set("Retune Speed",1);set("Humanize",0);set("Flex-Tune",0);set("Note Stabilizer",0);set("Scale Type",1);set("Scale Extension",0);
    set("Target Glide",motion ? 25.0/500.0 : 0);set("Formant Preserve",formants);set("Enabled",1);set("Low Latency (Experimental)",low);set("Voice Range",voice/5.0);set("Catch Low Notes",protect);
    int notes=0;
    if(vocal) {set("Root Note",root/11.0);set("Scale Type",0);}
    else {
        for(const auto& [name,id]:ids)if(name.size()>3 && name.substr(name.size()-3)==" On") {set(name,1);++notes;}
        assert(notes==12);
    }
    assert(processor->setProcessing(false)==kResultOk);assert(component->setActive(false)==kResultOk);assert(component->setActive(true)==kResultOk);assert(processor->setProcessing(true)==kResultOk);pump();
    const double floors[]={70,80,100,130,180,40};
    const double floor=protect ? std::min(80.,floors[voice]) : floors[voice];
    const int expected=low ? static_cast<int>(std::ceil(rate*.0019)) : static_cast<int>(std::ceil(1.3*std::ceil(rate/floor)))+8;
    assert(processor->getLatencySamples()==static_cast<uint32>(expected));
    const int frames=static_cast<int>(rate*(seconds+.5)),automationHop=rate/40;
    const double source=vocal ? 0 : floors[voice]*(motion ? 1.5 : 1.027),tau=6.2831853071795864769;
    std::vector<float> raw(static_cast<size_t>(frames)*2);double phase=0;
    if(vocal) {
        std::ifstream file(argv[10],std::ios::binary);assert(file);
        file.read(reinterpret_cast<char*>(raw.data()),static_cast<std::streamsize>(raw.size()*sizeof(float)));
        assert(file.gcount()==static_cast<std::streamsize>(raw.size()*sizeof(float)));
        for(float value:raw)assert(std::isfinite(value));
    }
    else for(int i=0;i<frames;++i) {
        phase+=tau*source*std::exp2(3*std::sin(tau*3*i/rate)/12)/rate;
        const double p=motion ? phase : tau*source*i/rate;
        raw[2*static_cast<size_t>(i)]=static_cast<float>(.16*std::sin(p)+.06*std::sin(3*p+.2)+.03*std::sin(11*p+.5));
        raw[2*static_cast<size_t>(i)+1]=static_cast<float>(.15*std::sin(p+.18)+.05*std::sin(4*p+.4)+.025*std::sin(13*p+.7));
    }
    std::vector<double> times,loads,cycles;const size_t capacity=frames/block+frames/automationHop+8;
    times.reserve(capacity);loads.reserve(capacity);cycles.reserve(capacity);
    // Optional host-side diagnostics only. Reserve before processing, capture
    // after the measured call, and write after all processing has finished.
    // Keep the ordinary timing run unchanged when this variable is absent.
    char tracePath[32768]{};
    const DWORD traceLength=GetEnvironmentVariableA("SKELLYTUNE_STRESS_TRACE",tracePath,sizeof(tracePath));
    assert(traceLength<sizeof(tracePath));
    struct CallbackTiming {int offset,frames;bool automated;double seconds;ULONG64 cycles;};
    std::vector<CallbackTiming> callbackTrace;
    if(traceLength)callbackTrace.reserve(capacity);
    Changes automation;automation.queue.id=ids.at("Formant Shift");
    const auto thread=GetCurrentThread();double total=0,totalCycles=0,peak=0;int measured=0,late=0;
    // Optional functional editor preview. Audio is processed silently, never sent to a device.
    // The GUI and paced audio worker use separate threads; no audio device is opened.
    const bool preview = GetEnvironmentVariableA("SKELLYTUNE_PREVIEW_UI", nullptr, 0) != 0;
    IPlugView* view = nullptr;
    HWND previewWindow = nullptr;
    if (preview) {
        view = controller->createView("editor"); assert(view);
        ViewRect bounds; assert(view->getSize(&bounds) == kResultOk);
        previewWindow = CreateWindowExW(0, L"STATIC", L"SkellyTune reactive preview",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, bounds.getWidth()+16, bounds.getHeight()+39,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        assert(previewWindow && view->attached(previewWindow, kPlatformTypeHWND) == kResultOk);
        assert(view->onSize(&bounds) == kResultOk);
        ShowWindow(previewWindow, SW_SHOW); UpdateWindow(previewWindow);
    }
    const auto previewStart = std::chrono::steady_clock::now();
    const auto renderLoop = [&] {
    for(int offset=0;offset<frames;) {
        const int n=vocal ? std::min(block,frames-offset)
                          : std::min({block,frames-offset,automationHop-offset%automationHop});
        for(int i=0;i<n;++i) {left[i]=raw[2*static_cast<size_t>(offset+i)];right[i]=raw[2*static_cast<size_t>(offset+i)+1];}
        data.numSamples=n;input.silenceFlags=output.silenceFlags=0;
        if(!vocal && offset%automationHop==0) {automation.queue.value=((offset/automationHop)&1) ? 1.0 : 0.0;data.inputParameterChanges=&automation;}else data.inputParameterChanges=nullptr;
        ULONG64 before=0,after=0;assert(QueryThreadCycleTime(thread,&before));
        const auto start=std::chrono::steady_clock::now();const auto status=processor->process(data);
        const double duration=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        assert(QueryThreadCycleTime(thread,&after));assert(status==kResultOk);
        if(measureStartup || offset>=rate/2) {times.push_back(duration);loads.push_back(duration*rate/n);cycles.push_back(static_cast<double>(after-before)/n);total+=duration;totalCycles+=after-before;measured+=n;late+=duration>static_cast<double>(n)/rate;}
        if(traceLength && (measureStartup || offset>=rate/2)) {
            assert(callbackTrace.size()<callbackTrace.capacity());
            callbackTrace.push_back({offset,n,data.inputParameterChanges!=nullptr,duration,after-before});
        }
        for(int i=0;i<n;++i) {assert(std::isfinite(outLeft[i]) && std::isfinite(outRight[i]));peak=std::max({peak,static_cast<double>(std::abs(outLeft[i])),static_cast<double>(std::abs(outRight[i]))});}
        offset+=n; if (!preview) pump();
        if (preview) std::this_thread::sleep_until(previewStart + std::chrono::microseconds(
            static_cast<int64_t>(1000000.0 * offset / rate)));
    }
    };
    if (preview) {
        // Model a DAW: painting/message handling must not run on the audio worker.
        std::atomic<bool> finished{false};
        std::thread audioWorker([&] { renderLoop(); finished.store(true, std::memory_order_release); });
        while (!finished.load(std::memory_order_acquire)) { pump(); Sleep(1); }
        audioWorker.join();
    } else renderLoop();
    if(traceLength) {
        std::ofstream trace(tracePath);assert(trace);
        trace.precision(17);
        trace<<"offset,frames,automated,seconds,thread_cycles,deadline_fraction\n";
        for(const auto& x:callbackTrace)
            trace<<x.offset<<','<<x.frames<<','<<x.automated<<','<<x.seconds<<','<<x.cycles<<','<<x.seconds*rate/x.frames<<'\n';
        assert(trace.good());
    }
    std::sort(times.begin(),times.end());std::sort(loads.begin(),loads.end());std::sort(cycles.begin(),cycles.end());
    const auto q=[](const auto& values,double percentile){return values[static_cast<size_t>(percentile*(values.size()-1))];};
    std::printf("rate,block,low,formants,range,source_hz,latency,duty,p99_seconds,max_seconds,p99_deadline_fraction,max_deadline_fraction,late_callbacks,callbacks,peak,pitch_motion,thread_cycles_per_frame,thread_p99_cycles_per_frame,input_profile,warmup_seconds,measured_seconds\n");
    std::printf("%d,%d,%d,%d,%d,%.8g,%d,%.8g,%.8g,%.8g,%.8g,%.8g,%d,%zu,%.8g,%d,%.8g,%.8g,%s,%.8g,%.8g\n",rate,block,low,formants,voice,source,expected,total*rate/measured,q(times,.99),times.back(),q(loads,.99),loads.back(),late,times.size(),peak,motion,totalCycles/measured,q(cycles,.99),vocal ? "vocal" : "synthetic",static_cast<double>(frames-measured)/rate,static_cast<double>(measured)/rate);
    std::ofstream manifest(std::string(argv[2])+"-native-profile.txt");
    if(vocal)manifest<<"VST3 component process; supplied stereo vocal; major root "<<root<<"; Catch Low Notes "<<protect<<"; full correction; Humanize 0; no color automation or artificial block splits; measurement duration recorded below\n";
    else manifest<<"VST3 component process; full correction; chromatic; Humanize 0; moving profile uses Target Glide 25ms; color +/-4st every 25ms; no wall-time conversion from thread cycles\n";
    manifest<<"warmup_seconds="<<static_cast<double>(frames-measured)/rate<<"; measured_seconds="<<static_cast<double>(measured)/rate<<"; measure_startup="<<measureStartup<<"\n";
    assert(processor->setProcessing(false)==kResultOk);assert(component->setActive(false)==kResultOk);
    if (preview) {
        Stream snapshot; assert(component->getState(&snapshot) == kResultOk);
        std::ofstream stateFile(std::string(argv[2])+"-state.bin", std::ios::binary);
        stateFile.write(snapshot.bytes.data(), static_cast<std::streamsize>(snapshot.bytes.size()));
        assert(stateFile.good());
        std::ofstream latencyFile(std::string(argv[2])+"-ui-final-latency.txt");
        latencyFile << processor->getLatencySamples();
    }
    if (view) { view->removed(); view->release(); DestroyWindow(previewWindow); }
    componentConnection->disconnect(controllerConnection);controllerConnection->disconnect(componentConnection);componentConnection->release();controllerConnection->release();
    controller->terminate();controller->release();processor->release();component->terminate();component->release();pump();factory->release();assert(finish());FreeLibrary(module);
}
