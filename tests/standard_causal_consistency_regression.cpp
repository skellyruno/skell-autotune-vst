#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static std::vector<float> render(double sr,int block,bool low,bool formant)
{
    humtune::PitchShifter sh;
    sh.prepare(sr,block);
    sh.setMinFrequency(100.0);
    sh.setLowLatencyExperiment(low);
    if(low) sh.setLowLatencyBudgetMilliseconds(1.9);
    sh.setPhaseLockedSource(true);
    sh.setFormantPreserve(!formant);
    sh.setFormantShiftSemitones(formant?2.0f:0.0f);
    const int n=static_cast<int>(sr*.48), half=n/2;
    std::vector<float> x(static_cast<size_t>(n));
    constexpr double pi=3.14159265358979323846;
    double phase=0;
    for(int i=0;i<n;++i){
        const double hz=i<half?165.0:246.94;
        phase+=2*pi*hz/sr;
        x[static_cast<size_t>(i)]=static_cast<float>(.17*(sin(phase)+.38*sin(2*phase+.3)+.18*sin(3*phase+.1)));
    }
    for(int i=0;i<n;){
        const int count=std::min({block,n-i,i<half?half-i:n-i});
        sh.process(x.data()+i,count,static_cast<float>(sr/(i<half?165.0:246.94)),i<half?.91f:1.13f);
        i+=count;
    }
    return x;
}
int main()
{
    for(double sr:{44100.0,48000.0,96000.0})
      for(bool low:{false,true})
       for(bool formant:{false,true}){
        const auto a=render(sr,32,low,formant),b=render(sr,511,low,formant);
        double error=0;
        for(size_t i=0;i<a.size();++i){
            if(!std::isfinite(a[i])||!std::isfinite(b[i]))return 2;
            error=std::max(error,std::abs(static_cast<double>(a[i]-b[i])));
        }
        std::printf("mode=%s sampleRate=%.0f formant=%d maxDiff=%.9g\n",low?"Low":"Standard",sr,formant,error);
        if(error>1e-6)return 1;
      }
    return 0;
}
