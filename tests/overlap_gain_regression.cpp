#include "PitchShifter.h"
#include <cassert>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <vector>
#ifdef NDEBUG
#error Assertions required
#endif

// A constant periodic source isolates the overlap envelope from waveform phase.
// Changing grain geometry must not create gain above the input's constant value.
static std::vector<float> render(double sr, int block, bool low)
{
    humtune::PitchShifter sh;
    sh.prepare(sr,block);sh.setMinFrequency(80);sh.setFormantPreserve(false);
    sh.setLowLatencyExperiment(low);
    if(low) sh.setLowLatencyBudgetMilliseconds(1.9);
    const int segment=static_cast<int>(sr*.09), total=segment*8;
    std::vector<float> out(static_cast<size_t>(total),.5f);
    for(int i=0;i<total;){
        const int section=i/segment;
        const double hz=section%2 ? 246.94 : 196.0;
        const float ratio=section%2 ? .91f : 1.12f;
        const int n=std::min({block,total-i,(section+1)*segment-i});
        sh.process(out.data()+i,n,static_cast<float>(sr/hz),ratio);i+=n;
    }
    double peak=0,energy=0;
    for(int i=segment;i<total;++i){
        const auto x=out[static_cast<size_t>(i)];assert(std::isfinite(x));
        peak=std::max(peak,static_cast<double>(std::abs(x)));energy+=x*x;
    }
    std::cout<<"overlap envelope sr="<<sr<<" low="<<low<<" peak="<<peak<<std::endl;
    assert(peak<=.5001);
    assert(std::sqrt(energy/(total-segment))>.48);
    return out;
}
int main(){
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true}){
        const auto a=render(sr,1,low);
        for(int block:{127,512}){
            const auto b=render(sr,block,low);assert(a==b);
        }
    }
}
