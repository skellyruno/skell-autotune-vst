#include "AutoTuneEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

struct Result {
    std::vector<humtune::AutoTuneEngine::HopTrace> hops;
    std::vector<float> audio;
    int latency = 0;
};
static Result render(int block, bool attack)
{
    constexpr int sr=48000;
    constexpr int n=sr*2;
    Result r; r.audio.resize(n);
    humtune::AutoTuneEngine e;
    e.setVoiceRange(2); e.setRootNote(0); e.setScaleType(0);
    e.setLowLatencyMode(true); e.setRetuneSpeed(0); e.setNoteStabilizer(true);
    e.prepare(sr,block);
    r.latency=e.getLatencySamples();
    r.hops.reserve(1024);
    e.setOfflineTraceCallback([](void* p, const humtune::AutoTuneEngine::HopTrace& t) noexcept {
        auto& dst=static_cast<Result*>(p)->hops;
        if (dst.size()<dst.capacity()) dst.push_back(t);
    }, &r);
    constexpr double pi=3.14159265358979323846;
    double phase=0;
    for(int i=0;i<n;++i)
    {
        const bool newNote=i>=sr;
        const double f=newNote?261.625565:220.0;
        const double level=newNote?(attack?0.50:0.22):0.22;
        phase += 2*pi*f/sr;
        r.audio[i]=static_cast<float>(level*std::sin(phase));
    }
    for(int pos=0;pos<n;pos+=block)
    {
        const int sz=std::min(block,n-pos);
        juce::AudioBuffer<float> b(1,sz);
        for(int i=0;i<sz;++i) b.getWritePointer(0)[i]=r.audio[pos+i];
        e.process(b);
        for(int i=0;i<sz;++i) r.audio[pos+i]=b.getReadPointer(0)[i];
    }
    return r;
}
int main()
{
    const auto a=render(64,true), b=render(511,true), c=render(64,false);
    bool good=a.latency==92 && b.latency==a.latency && a.hops.size()==b.hops.size();
    double maxWaveformDiff=0;
    for(size_t i=0;i<a.audio.size();++i)
        maxWaveformDiff=std::max(maxWaveformDiff,double(std::abs(a.audio[i]-b.audio[i])));
    int onsetCount=0, firstAccepted=-1, firstWithoutAttack=-1;
    float maxGain=0;int badTraces=0;
    for (const auto& t:a.hops)
    {
        onsetCount+=t.onset;
        if (firstAccepted<0 && t.sample>=48000 && t.selectedTargetMidi>=60.f)
            firstAccepted=static_cast<int>(t.sample);
        maxGain=std::max(maxGain,t.grainGain);
        if (!std::isfinite(t.grainCoverage)||!std::isfinite(t.predictedMidi)
             ||!std::isfinite(t.maxOutputStep)||t.grainGain<0)
            ++badTraces;
    }
    for (const auto& t:c.hops)
        if (firstWithoutAttack<0 && t.sample>=48000 && t.selectedTargetMidi>=60.f)
            firstWithoutAttack=static_cast<int>(t.sample);
    // Due to the detector's historic-window lag, onset must be retained until
    // the new note appears in the pitch trace; this must not add a new delay.
    good &= maxWaveformDiff < .0001 && badTraces==0 && onsetCount>0
         && firstAccepted>=48000 && firstWithoutAttack>=48000
         && firstAccepted + 192 <= firstWithoutAttack;
    std::printf("onset trace: maxDiff=%.8f onset=%d firstWithAttack=%d firstNoAttack=%d gainMax=%.2f errors=%d -> %s\n",
        maxWaveformDiff,onsetCount,firstAccepted,firstWithoutAttack,maxGain,badTraces,
        good?"PASS":"FAIL");
    return good?0:1;
}
