#include "PitchShifter.h"
#ifdef NDEBUG
#error "Regression assertions must stay enabled in Release"
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static std::vector<float> render(double sr, int block, bool lowLatency, bool compensate)
{
    humtune::PitchShifter s;
    s.prepare(sr,block);
    s.setMinFrequency(100);
    s.setLowLatencyExperiment(lowLatency);
    if (lowLatency) s.setLowLatencyBudgetMilliseconds(1.9);
    s.setFormantPreserve(false);
    s.setPhaseLockedSource(true);
    s.setEnergyMatchedHandoffEnabled(compensate);
    const int n=static_cast<int>(sr*1.25);
    const int t1=static_cast<int>(sr*.34), t2=static_cast<int>(sr*.405);
    const int t3=static_cast<int>(sr*.70), t4=static_cast<int>(sr*.755);
    unsigned int noiseState=14723;
    double phase=0.0;
    std::vector<float> audio(static_cast<size_t>(n));
    for(int i=0;i<n;++i)
    {
        const double f=i<t3?220.0:287.0;
        phase+=2.0*3.141592653589793*f/sr;
        noiseState=1664525u*noiseState+1013904223u;
        const float noise=static_cast<float>(noiseState>>8)/16777216.0f-0.5f;
        const bool consonant=(i>=t1&&i<t2)||(i>=t3&&i<t4);
        audio[static_cast<size_t>(i)]=consonant ? 0.3f*noise
            : static_cast<float>(0.21*sin(phase)+0.09*sin(2*phase+.3)+0.04*sin(3*phase));
    }
    for(int pos=0;pos<n;)
    {
        int sz=std::min(block,n-pos);
        for(int event:{t1,t2,t3,t4})
            if(pos<event) sz=std::min(sz,event-pos);
        const bool dry=(pos>=t1&&pos<t2)||(pos>=t3&&pos<t4);
        s.setDryPassthrough(dry);
        const float ratio=pos<t3 ? 1.0594631f : 0.9438743f;
        const float period=static_cast<float>(sr/(pos<t3?220.0:287.0));
        s.process(audio.data()+pos,sz,period,ratio);
        pos+=sz;
    }
    return audio;
}
int main()
{
    for(double sr:{44100.,48000.,96000.}) for(bool low:{false,true})
    {
        const auto baseline=render(sr,127,low,false);
        const auto a=render(sr,127,low,true);
        const auto b=render(sr,512,low,true);
        assert(a.size()==b.size());
        double maxDifference=0.0, maxPeak=0.0, diffFromBaseline=0.0;
        for(size_t i=0;i<a.size();++i)
        {
            assert(std::isfinite(a[i]));
            maxDifference=std::max(maxDifference,std::abs(double(a[i])-b[i]));
            diffFromBaseline=std::max(diffFromBaseline,std::abs(double(a[i])-baseline[i]));
            maxPeak=std::max(maxPeak,std::abs(double(a[i])));
        }
        assert(maxDifference<1.0e-5); // host callback partition does not change sound
        assert(maxPeak<1.5);         // bounded gain on difficult transients
        // Both modes now compensate covariance loss at a voicing handoff.
        assert(diffFromBaseline>1.0e-5);
        std::printf("%.0f Hz mode %d: hostDiff %.9f, maxPeak %.4f, compensationDiff %.6f\n",
            sr,low,maxDifference,maxPeak,diffFromBaseline);
    }
}
