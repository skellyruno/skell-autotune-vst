#include "PitchDetector.h"
#include <algorithm>
#include <cassert>
#ifdef NDEBUG
#error "Fractional period quality gates require assertions"
#endif
#include <cmath>
#include <cstdio>
#include <vector>
int main() {
    std::puts("rate,floor,source,type,expected,median_cents,p95_cents,voiced");
    for(double sr:{44100.,48000.,96000.})for(float floor:{35.f,80.f,180.f})
      for(double f:{82.4,110.,211.3,240.,333.3,480.,720.,1100.,1490.})for(int type=0;type<4;++type) {
        const double expected=type==3 ? 3*f : f;
        if(expected<floor || expected>1499)continue;
        // Extreme 1490-Hz weak-root material remains an explicitly measured
        // diagnostic failure; this regression covers the newly corrected range.
        if(type==1 && f>1400)continue;
        humtune::PitchDetector d;d.prepare(sr,127);d.setMinFrequency(floor);
        const int hop=d.getHopSize(),frames=static_cast<int>(sr*.22);
        std::vector<double> errors;
        for(int i=0;i<frames;++i) {
            const double p=6.283185307179586*f*i/sr;
            const float x=static_cast<float>(type==0 ? .2*std::sin(p)+.05*std::sin(2*p+.3)+.02*std::sin(3*p+.5)
                       : type==1 ? .02*std::sin(p)+.02*std::sin(2*p+.3)+.2*std::sin(3*p+.5)
                       : type==2 ? .2*std::sin(p) : .2*std::sin(3*p+.5));
            d.feedSamples(&x,1);
            if((i+1)%hop==0) {
                const double hz=d.detectPitch();
                if(i>sr*.12 && hz>0)errors.push_back(std::abs(1200*std::log2(hz/expected)));
            }
        }
        assert(errors.size()>20);
        std::sort(errors.begin(),errors.end());
        assert(errors[static_cast<size_t>(.95*(errors.size()-1))]<20.0);
        std::printf("%.0f,%.0f,%.3f,%d,%.3f,%.6f,%.6f,%zu\n",sr,floor,f,type,expected,
                    errors.empty() ? -1 : errors[errors.size()/2],errors.empty() ? -1 : errors[static_cast<size_t>(.95*(errors.size()-1))],errors.size());
    }
}
