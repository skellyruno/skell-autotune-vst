#include "PitchShifter.h"
#include <cmath>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <algorithm>

static void setup(humtune::PitchShifter& p,double sr, bool low) {
    p.setFormantPreserve(false);
    p.prepare(sr,127); p.setMinFrequency(100);p.setLowLatencyExperiment(low);
    if(low) p.setLowLatencyBudgetMilliseconds(1.9);
    p.setPhaseLockedSource(true);p.setFormantPreserve(false);
}
static std::vector<float> makeSignal(int n,double sr,int kind) {
    std::vector<float> x(n);
    double phase=0;uint32_t rng=0x1524aab;
    for(int i=0;i<n;++i){
        double hz=i<n/2?190.0:296.0;
        phase+=2*3.141592653589793*hz/sr;
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
        if(kind==0) x[i]=static_cast<float>(0.29*std::sin(phase)+0.16*std::sin(phase*2+0.3));
        else x[i]=static_cast<float>(((rng/4294967296.0)*2-1)*0.38);
    }
    return x;
}
static void run(humtune::PitchShifter& p,std::vector<float>& a, int block, double sr){
    int at=0;while(at<static_cast<int>(a.size())){
        const int count=std::min(block,static_cast<int>(a.size())-at);
        const float pitch=at<static_cast<int>(a.size())/2 ? 190.f:296.f;
        p.process(a.data()+at,count,static_cast<float>(sr/pitch), 1.13f);
        at+=count;
    }
}
int main(){
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true}){
        auto clean=makeSignal(static_cast<int>(sr*.3),sr,0);
        auto a=clean, b=clean;
        humtune::PitchShifter fresh, dirty;
        setup(fresh,sr,low);setup(dirty,sr,low);
        auto noise=makeSignal(static_cast<int>(sr*.8),sr,1);
        run(dirty,noise,127,sr);
        dirty.reset();
        run(fresh,a,127,sr);run(dirty,b,127,sr);
        double worst=0.0;
        for(size_t i=0;i<a.size();++i){
            if(!std::isfinite(a[i])||!std::isfinite(b[i]))return 3;
            worst=std::max(worst,std::abs(double(a[i])-b[i]));
        }
        std::printf("source history reset: sr=%.0f low=%d max diff=%.9g\n",sr,low?1:0,worst);
        if(worst>1e-6) { for(size_t i=0,printed=0;i<a.size() && printed<10;++i)if(std::abs(a[i]-b[i])>0.00001f){std::printf("first at %zu clean=%g dirty=%g\n",i,a[i],b[i]);++printed;} return 2; }
    }
}
