#include "AutoTuneEngine.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef NDEBUG
#error "Weak fundamental evidence tests require assertions"
#endif
namespace {
struct Result {std::vector<float> audio;int bad=0,correct=0,rejected=0;double first=-1;};
Result render(double sr,bool low,double weak,double jump,int block,double thirdPhase) {
    humtune::AutoTuneEngine engine;engine.setVoiceRange(4);engine.setRangeProtection(true);
    engine.setScaleType(2);std::array<bool,12> chromatic;chromatic.fill(true);engine.setCustomScale(chromatic);
    engine.setRetuneSpeed(0);engine.setAmount(1);engine.setNoteStabilizer(false);
    engine.setLowLatencyMode(low);engine.prepare(sr,block);
    struct Context {double sr,jump;Result* result;} context{sr,jump,nullptr};
    Result result;context.result=&result;
    result.audio.resize(static_cast<size_t>(sr*.7));
    engine.setOfflineTraceCallback([](void* pointer,const humtune::AutoTuneEngine::HopTrace& t)noexcept {
        const auto& c=*static_cast<Context*>(pointer);const double time=t.sample/c.sr;
        if(time>.46 && time<.68) {const bool correct=t.observedHz>0 && std::abs(t.observedHz/240-1)<.01; c.result->bad+=!correct;c.result->correct+=correct;c.result->rejected+=t.rejectedUpperHarmonic;}
        if(c.jump>0 && time>=.4 && std::abs(t.observedHz/c.jump-1)<.05 && c.result->first<0)c.result->first=time-.4;
    },&context);
    double phase=0;
    for(int at=0;at<static_cast<int>(result.audio.size());at+=block) {
        const int n=std::min(block,static_cast<int>(result.audio.size())-at);juce::AudioBuffer<float> buffer(1,n);
        for(int i=0;i<n;++i) {
            const double time=(at+i)/sr,blend=std::clamp((time-.4)/.005,0.0,1.0);
            phase+=6.283185307179586*(jump>0 && time>=.4 ? jump : 240)/sr;
            const double fundamental=jump>0 ? .2 : .2*(1-blend)+weak*blend;
            const double second=jump>0 ? .05 : .05*(1-blend)+weak*blend;
            const double third=jump>0 ? .02 : .02*(1-blend)+.2*blend;
            buffer.getWritePointer(0)[i]=static_cast<float>(fundamental*std::sin(phase)+second*std::sin(2*phase+.3)+third*std::sin(3*phase+thirdPhase));
        }
        engine.process(buffer);
        std::copy_n(buffer.getReadPointer(0),n,result.audio.begin()+at);
    }
    return result;
}
}
int main() {
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true}) {
        for(double weak:{.02,.025,.03})for(double phase:{0.0,.7,2.1}) {
            const auto result=render(sr,low,weak,0,127,phase);
            std::printf("weak-fundamental sr=%.0f low=%d weak=%.3f phase=%.1f false=%d rejected=%d\n",sr,low,weak,phase,result.bad,result.rejected);
            assert(result.bad==0 && result.correct>40);
            for(float x:result.audio)assert(std::isfinite(x));
        }
        const auto result=render(sr,low,.02,0,127,.7);
        for(int block:{1,512})assert(result.audio==render(sr,low,.02,0,block,.7).audio);
        for(double jump:{480.,720.,960.}) {
            const auto result=render(sr,low,.02,jump,127,.7);
            std::printf("genuine register sr=%.0f low=%d target=%.0f delay=%.6f\n",sr,low,jump,result.first);
            assert(result.first>=0 && result.first<.025);
        }
    }
}
