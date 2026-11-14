#include "PitchShifter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#ifdef NDEBUG
#error "Independent spectral oracle requires active assertions"
#endif
namespace humtune {
struct PitchShifterSpectralOracle {
    static void check(double sr,int oldCount,int currentCount,double frequency,double blend,bool cache) {
        PitchShifter p;p.prepare(sr,127);
        p.liveOldIndex=0;p.liveCurrentIndex=1;p.liveSpareIndex=2;
        p.liveOldCount=oldCount;p.liveCurrentCount=currentCount;
        p.liveSpectrumValid=true;p.liveSpectrumBlend=blend;
        for(int bank=0;bank<2;++bank) {
            float* c=p.liveCoefficients(bank);
            const int count=bank==0 ? oldCount : currentCount;
            for(int h=0;h<=PitchShifter::kLiveHarmonics;++h) {
                c[h]=static_cast<float>(.02*std::cos(h*1.73+bank*.4)/(1+.1*h));
                c[PitchShifter::kLiveHarmonics+1+h]=h==0 ? 0.0f : static_cast<float>(.02*std::sin(h*2.13+bank*.6)/(1+.1*h));
            }
            if(cache)p.buildLiveWaveTable(bank,count,frequency);
        }
        double maximum=0;
        for(int j=0;j<41;++j) {
            const double phase=j==40 ? std::nextafter(1.0,0.0) : j*.02471685317;
            // Deliberately evaluate a nearby clock to audit the cache tolerance.
            const double f=frequency*(j%2 ? 1+1e-7 : 1);
            const long double ceiling=std::min(24000.0,.49*sr),roll=std::max(500.0,.05*static_cast<double>(ceiling));
            long double expected=(1-blend)*p.liveCoefficients(0)[0]+blend*p.liveCoefficients(1)[0];
            for(int h=1;h<=std::max(oldCount,currentCount);++h) {
                const long double band=std::clamp((ceiling-h*f)/roll,0.0L,1.0L);
                long double real=0,imag=0;
                for(int bank=0;bank<2;++bank) {
                    if(h>(bank==0 ? oldCount : currentCount))continue;
                    const auto* c=p.liveCoefficients(bank);const double w=bank==0 ? 1-blend : blend;
                    real+=w*c[h];imag+=w*c[PitchShifter::kLiveHarmonics+1+h];
                }
                const long double angle=6.283185307179586476925286766559L*h*phase;
                expected+=2*band*(real*std::cos(angle)-imag*std::sin(angle));
            }
            const double actual=p.evaluateLiveSpectrum(phase,f);
            assert(std::isfinite(actual));
            maximum=std::max(maximum,std::abs(actual-static_cast<double>(expected)));
        }
        std::printf("spectral oracle sr=%.0f counts=%d/%d f=%.2f blend=%.2f cache=%d max=%.9g\n",sr,oldCount,currentCount,frequency,blend,cache,maximum);
        std::fflush(stdout);
        assert(maximum<(cache ? 3e-5 : 3e-7));
    }
};
}
int main() {
    for(double sr:{44100.0,48000.0,96000.0})
      for(int count:{1,17,127,511,2048})
       for(double frequency:{35.1,82.4,211.3,713.7,1499.0})
        for(double blend:{0.0,.31,1.0})
         for(bool cache:{false,true})
            humtune::PitchShifterSpectralOracle::check(sr,count,std::max(1,count*3/4),frequency,blend,cache);
}
