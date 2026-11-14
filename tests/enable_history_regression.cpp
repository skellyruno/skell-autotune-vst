#include "AutoTuneEngine.h"
#include <cstdio>
#include <cmath>
#include <cassert>
#ifdef NDEBUG
#error Active assertions required
#endif
struct Capture {bool monitor=false;int hops=0,stale=0;};
int main(){
    #ifndef SKELLYTUNE_TEST_HISTORY_ONLY
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true}){
        humtune::AutoTuneEngine e;e.setVoiceRange(4);e.setRangeProtection(true);e.setLowLatencyMode(low);e.prepare(sr,127);
        const float before=e.getMinFrequency();e.setEnabled(false);e.setEnabled(true);
        std::printf("protected range sr=%.0f low=%d before=%.1f after=%.1f\n",sr,low,before,e.getMinFrequency());std::fflush(stdout);
        assert(before==80 && e.getMinFrequency()==before);
    }
    #endif
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})for(int block:{1,127,512})for(double nextHz:{220.,330.}){
        humtune::AutoTuneEngine e;e.setVoiceRange(2);e.setRangeProtection(false);e.setRootNote(9);e.setScaleType(0);e.setRetuneSpeed(0);e.setHumanize(0);e.setFormantPreserve(false);e.setLowLatencyMode(low);e.prepare(sr,block);
        Capture capture;e.setOfflineTraceCallback([](void* p,const humtune::AutoTuneEngine::HopTrace& t) noexcept {auto& c=*static_cast<Capture*>(p);if(c.monitor){if(c.hops<2)c.stale+=t.observedHz>0||t.voiced;++c.hops;}},&capture);
        double phase=0;const auto play=[&](double hz,int frames){for(int at=0;at<frames;at+=block){const int n=std::min(block,frames-at);juce::AudioBuffer<float> b(2,n);for(int i=0;i<n;++i){const float x=static_cast<float>(.2*std::sin(phase)+.04*std::sin(2*phase+.2));phase+=6.283185307179586*hz/sr;for(int ch=0;ch<2;++ch)b.getWritePointer(ch)[i]=x;}e.process(b);}};
        play(220,static_cast<int>(sr*.5));assert(std::abs(e.getDetectedHz()-220)<2);const int latency=e.getLatencySamples();
        e.setEnabled(false);play(nextHz,static_cast<int>(sr*.1));capture.monitor=true;e.setEnabled(true);play(nextHz,static_cast<int>(sr*.08));
        std::printf("fresh history sr=%.0f low=%d block=%d next=%.0f stale=%d hops=%d detected=%.3f latency=%d\n",sr,low,block,nextHz,capture.stale,capture.hops,e.getDetectedHz(),e.getLatencySamples());std::fflush(stdout);
        assert(capture.hops>=8&&capture.stale==0);assert(std::abs(e.getDetectedHz()-nextHz)<2);assert(latency==e.getLatencySamples());
    }
}
