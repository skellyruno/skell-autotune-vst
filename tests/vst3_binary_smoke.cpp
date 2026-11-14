#include <windows.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

// Loads the real VST3 binary, creates its component, and processes stereo audio.
// This is an SDK smoke host, not FL Studio or a full VST3 validator.
int main(int argc,char** argv)
{
    using namespace Steinberg;
    using namespace Steinberg::Vst;
    if(argc!=2)return 2;
    const auto module=LoadLibraryA(argv[1]);
    if(!module){std::cerr<<"LoadLibrary failed "<<GetLastError()<<"\n";return 3;}
    const auto init=reinterpret_cast<bool(*)()>(GetProcAddress(module,"InitDll"));
    const auto exit=reinterpret_cast<bool(*)()>(GetProcAddress(module,"ExitDll"));
    const auto getFactory=reinterpret_cast<IPluginFactory*(*)()>(GetProcAddress(module,"GetPluginFactory"));
    assert(init&&exit&&getFactory&&init());
    auto* factory=getFactory();assert(factory);
    IComponent* component=nullptr;
    for(int i=0;i<factory->countClasses();++i){
        PClassInfo info{};assert(factory->getClassInfo(i,&info)==kResultOk);
        if(std::string(info.category)==kVstAudioEffectClass)
            assert(factory->createInstance(info.cid,IComponent_iid,reinterpret_cast<void**>(&component))==kResultOk);
    }
    assert(component&&component->initialize(nullptr)==kResultOk);
    IAudioProcessor* processor=nullptr;
    assert(component->queryInterface(IAudioProcessor_iid,reinterpret_cast<void**>(&processor))==kResultOk);
    SpeakerArrangement stereo=SpeakerArr::kStereo;
    assert(processor->setBusArrangements(&stereo,1,&stereo,1)==kResultOk);
    assert(component->activateBus(kAudio,kInput,0,true)==kResultOk);
    assert(component->activateBus(kAudio,kOutput,0,true)==kResultOk);
    ProcessSetup setup{};setup.processMode=kRealtime;setup.symbolicSampleSize=kSample32;setup.maxSamplesPerBlock=512;setup.sampleRate=48000;
    assert(processor->setupProcessing(setup)==kResultOk);
    assert(component->setActive(true)==kResultOk);
    assert(processor->setProcessing(true)==kResultOk);
    std::vector<float> left(512),right(512),outLeft(512),outRight(512);
    float* inputs[]={left.data(),right.data()};float* outputs[]={outLeft.data(),outRight.data()};
    AudioBusBuffers input{},output{};input.numChannels=output.numChannels=2;input.channelBuffers32=inputs;output.channelBuffers32=outputs;
    ProcessData data{};data.processMode=kRealtime;data.symbolicSampleSize=kSample32;data.numSamples=512;data.numInputs=data.numOutputs=1;data.inputs=&input;data.outputs=&output;
    double energy=0;
    for(int block=0;block<120;++block){
        for(int i=0;i<512;++i)left[i]=right[i]=static_cast<float>(.18*std::sin(6.283185307179586*207*(block*512+i)/48000));
        assert(processor->process(data)==kResultOk);
        for(int i=0;i<512;++i){assert(std::isfinite(outLeft[i])&&std::isfinite(outRight[i]));energy+=outLeft[i]*outLeft[i]+outRight[i]*outRight[i];}
    }
    assert(energy>1);
    std::cout<<"Real VST3 factory/component/stereo process passed; latency="<<processor->getLatencySamples()<<" energy="<<energy<<std::endl;
    assert(processor->setProcessing(false)==kResultOk);assert(component->setActive(false)==kResultOk);
    processor->release();assert(component->terminate()==kResultOk);component->release();factory->release();
    assert(exit());FreeLibrary(module);
}
