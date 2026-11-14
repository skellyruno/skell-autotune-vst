#include "PitchShifter.h"
#include "SteadyNoteForecast.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <iostream>

#ifndef NDEBUG
#else
#error "Quality regression must keep asserts enabled"
#endif

namespace {
std::vector<float> synth(double sampleRate, bool low, int block, int& latency)
{
    humtune::PitchShifter shifter;
    shifter.prepare(sampleRate, block);
    shifter.setMinFrequency(80.0);
    shifter.setLowLatencyExperiment(low);
    if (low) shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setPhaseLockedSource(true);
    shifter.setFormantPreserve(false);
    latency=shifter.getLatencySamples();
    constexpr double pi=3.141592653589793;
    const int total=static_cast<int>(sampleRate * 0.72);
    const int firstChange=static_cast<int>(sampleRate * 0.24);
    const int secondChange=static_cast<int>(sampleRate * 0.48);
    std::vector<float> out(static_cast<size_t>(total));
    double phase=0.0;
    for (int n=0;n<total;++n) {
        const double hz = n<firstChange?164.81:(n<secondChange?220.0:293.66);
        phase+=2.0*pi*hz/sampleRate;
        out[static_cast<size_t>(n)] = static_cast<float>(0.18*std::sin(phase)
            +0.06*std::sin(2.0*phase+0.18)+0.032*std::sin(3.0*phase+0.34));
    }
    for (int n=0;n<total;) {
        const int next = n<firstChange?firstChange:(n<secondChange?secondChange:total);
        const int count=std::min(block,next-n);
        assert(count>0);
        const double hz=n<firstChange?164.81:(n<secondChange?220.0:293.66);
        shifter.process(out.data()+n,count,static_cast<float>(sampleRate/hz),
                        n<firstChange?1.11f:(n<secondChange?0.91f:1.07f));
        n+=count;
    }
    return out;
}
}

int main()
{
    humtune::SteadyNoteForecast predictor;
    const float observed=60.1f,predicted=60.3f;
    for (int i=0;i<20;++i)
        predictor.process(observed,predicted,60.0f,0.98f,0.004,false,false);
    const float steady=predictor.process(observed,predicted,60.0f,0.98f,0.004,false,false);
    assert(std::abs(steady - (observed+0.46f*(predicted-observed))) < 1e-5f);
    assert(std::abs(predictor.process(observed,predicted,60.0f,0.98f,0.004,false,true)-predicted)<1e-5f);
    assert(std::abs(predictor.process(observed,predicted,62.0f,0.98f,0.004,false,false)-predicted)<1e-5f);
    for (int i=0;i<20;++i)
        assert(std::abs(predictor.process(observed,predicted,62.0f,0.98f,0.004,true,false)-predicted)<1e-5f);
    for (double sr:{44100.0,48000.0,96000.0})
      for (bool low:{false,true}) {
        int aDelay=0,bDelay=0;
        const auto a=synth(sr,low,47,aDelay);
        const auto b=synth(sr,low,509,bDelay);
        assert(a.size()==b.size() && aDelay==bDelay);
        double power=0.0;float largest=0.0f;
        for (size_t i=0;i<a.size();++i) {
            assert(std::isfinite(a[i]) && std::isfinite(b[i]));
            assert(std::abs(a[i]-b[i]) <= 1.0e-5f);
            power+=static_cast<double>(a[i])*a[i];
            largest=std::max(largest,std::abs(a[i]));
        }
        assert(power/a.size()>0.0002);
        assert(largest<1.5f);
        std::cout << "v39 quality sr="<<sr<<" low="<<low
                  <<" delay="<<aDelay<<" peak="<<largest<<"\n";
      }
}
