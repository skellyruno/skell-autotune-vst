#include "PitchShifter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef NDEBUG
#error "Release regression assertions must stay active"
#endif
namespace {
constexpr double pi=3.141592653589793;
std::vector<float> render(double sr,double hz,bool preserve,int block,int& latency,bool low)
{
    humtune::PitchShifter shifter;
    shifter.prepare(sr,block);
    shifter.setMinFrequency(80);
    shifter.setLowLatencyExperiment(low);
    shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(preserve);
    shifter.setPhaseLockedSource(true);
#ifndef SKELLYTUNE_TEST_NO_EPOCH_API
    shifter.setSourceEpochTrackingEnabled(true);
#endif
    latency=shifter.getLatencySamples();
    std::vector<float> audio(static_cast<size_t>(sr));
    for(size_t i=0;i<audio.size();++i)
    {
        const double phase=2*pi*hz*i/sr;
        audio[i]=static_cast<float>(0.2*std::sin(phase)+0.07*std::sin(2*phase+0.3));
    }
    for(int offset=0;offset<static_cast<int>(audio.size());offset+=block)
        shifter.process(audio.data()+offset,std::min(block,static_cast<int>(audio.size())-offset),
                        static_cast<float>(sr/hz),1);
    return audio;
}
}
int main()
{
    for(bool low:{false,true})
    for(double sr:{44100.0,48000.0,96000.0})
        for(double hz:{110.0,220.0,440.0})
            for(bool preserve:{false,true})
            {
                int latency=0,other=0;
                const auto wet=render(sr,hz,preserve,127,latency,low);
                assert(wet==render(sr,hz,preserve,512,other,low));
                assert(latency==other);
                const int radius=static_cast<int>(sr*0.0011);
                double best=-2;
                int measured=0;
                for(int lag=std::max(0,latency-radius);lag<=latency+radius;++lag)
                {
                    double xy=0,xx=0,yy=0;
                    for(int i=static_cast<int>(sr*0.3);i<static_cast<int>(sr*0.8);i+=4)
                    {
                        const double phase=2*pi*hz*(i-lag)/sr;
                        const double dry=0.2*std::sin(phase)+0.07*std::sin(2*phase+0.3);
                        const double y=wet[static_cast<size_t>(i)];
                        assert(std::isfinite(y));
                        xy+=dry*y;xx+=dry*dry;yy+=y*y;
                    }
                    const double correlation=xy/std::sqrt(xx*yy);
                    if(correlation>best){best=correlation;measured=lag;}
                }
                std::printf("live unity sr=%.0f hz=%.0f formants=%d: reported=%d phaseLag=%d correlation=%.7f\n",
                            sr,hz,preserve,latency,measured,best);
                // This checks neutral-tone phase, not a claim of fixed perceptual
                // delay for every resynthesized transient or non-unity ratio.
                assert(std::abs(measured-latency)<=3);
                assert(best>0.998);
            }
}
