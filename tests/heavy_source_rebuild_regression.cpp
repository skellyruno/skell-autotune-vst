#include "PitchShifter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#ifdef NDEBUG
#error "Source scheduling evidence needs active assertions"
#endif
namespace humtune {
// Reuse the existing test-only spectral friend to inspect actual build events;
// no instrumentation or counters are added to the audio callback.
struct PitchShifterSpectralOracle {
    static int check(double sr,bool low) {
        PitchShifter s;s.prepare(sr,127);s.setMinFrequency(35);
        s.setPhaseLockedSource(true);s.setFormantPreserve(true);
        s.setLowLatencyExperiment(low);s.setLowLatencyBudgetMilliseconds(1.9);
        const int length=static_cast<int>(sr*.65),automation=static_cast<int>(sr/40);
        int builds=0,heavy=0,violations=0,pendingSamples=0,maxPending=0;
        double phase=0,maxAge=0,energy=0,peak=0;
        for(int i=0;i<length;++i) {
            if(i%automation==0)s.setFormantShiftSemitones((i/automation)&1 ? 4.f : -4.f);
            const double time=i/sr;
            const double hz=40*std::exp2(3*std::sin(6.283185307179586*3*time)/12);
            phase+=6.283185307179586*hz/sr;
            float sample=static_cast<float>(.16*std::sin(phase)+.06*std::sin(3*phase+.2)+.03*std::sin(11*phase+.5));
            const auto previous=s.liveLastBuild;const bool wasValid=s.liveSpectrumValid;
            const bool wasHeavy=s.liveCurrentCount>=1023;
            s.process(&sample,1,static_cast<float>(sr/hz),static_cast<float>(43.65352893/hz));
            assert(std::isfinite(sample));peak=std::max(peak,static_cast<double>(std::abs(sample)));
            energy+=sample*sample;
            if(s.liveLastBuild!=previous) {
                ++builds;
                if(wasValid && wasHeavy) {
                    ++heavy;
                    violations+=s.liveLastBuild-previous<s.liveControlSamples;
                }
            }
            if(s.liveSpectrumValid && s.recordedEpoch!=s.liveLastSourceEpoch)++pendingSamples;
            else pendingSamples=0;
            maxPending=std::max(maxPending,pendingSamples);
            maxAge=std::max(maxAge,static_cast<double>(s.getSourceCycleDiagnostics().ageSeconds));
            assert(!s.recordedEpochValid || s.recordedEpoch<s.inCount);
        }
        std::printf("source schedule sr=%.0f low=%d builds=%d heavy=%d violations=%d pending=%d age=%.6f peak=%.6f rms=%.6f\n",sr,low,builds,heavy,violations,maxPending,maxAge,peak,std::sqrt(energy/length));
        std::fflush(stdout);
        assert(builds>100 && heavy>20);
        assert(maxPending<s.liveControlSamples);
        assert(maxAge<.10 && peak<.6 && energy/length>.001);
        return violations;
    }
};
}
int main() {
    int violations=0;
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})
        violations+=humtune::PitchShifterSpectralOracle::check(sr,low);
    assert(violations==0);
}
