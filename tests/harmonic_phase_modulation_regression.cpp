#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <cassert>
#ifdef NDEBUG
#error "Harmonic-phase regression requires active assertions"
#endif
int main() {
    constexpr double pi=3.14159265358979323846;
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})for(bool formants:{false,true}) {
        const int period=static_cast<int>(std::lround(sr/246.9416506));const double target=sr/period;
        humtune::PitchShifter sh;sh.prepare(sr,127);sh.setMinFrequency(80);sh.setPhaseLockedSource(true);
        sh.setLowLatencyExperiment(low);sh.setLowLatencyBudgetMilliseconds(1.9);sh.setFormantPreserve(formants);
        std::vector<float> audio(static_cast<size_t>(sr*1.5));double inputPower=0;
        for(int i=0;i<static_cast<int>(audio.size());++i) {
            const double t=i/sr,p=2*pi*240*t;
            const double value=.02*std::sin(p)+.2*std::sin(2*p+.9*std::sin(2*pi*5*t))+.05*std::sin(3*p+.5);
            audio[static_cast<size_t>(i)]=static_cast<float>(value);if(i>sr*.3)inputPower+=value*value;
        }
        for(int i=0;i<static_cast<int>(audio.size());i+=127)
            sh.process(audio.data()+i,std::min(127,static_cast<int>(audio.size())-i),static_cast<float>(sr/240),static_cast<float>(target/240));
        std::vector<double> errors;double previous=0,peak=0,power=0;bool have=false;
        const int half=10*period,hop=2*period;
        // Coherent 20-cycle Blackman analysis suppresses leakage from the
        // deliberately much stronger second harmonic into the H1 phase fit.
        for(int center=static_cast<int>(sr*.35);center<static_cast<int>(sr*1.35);center+=hop) {
            double re=0,im=0;
            for(int j=-half;j<=half;++j) {
                const double angle=2*pi*target*(center+j)/sr;
                const double weight=.42+.5*std::cos(pi*j/half)+.08*std::cos(2*pi*j/half);
                const double value=audio[static_cast<size_t>(center+j)];re+=weight*value*std::cos(angle);im-=weight*value*std::sin(angle);
            }
            const double phase=std::atan2(im,re);
            if(have) {const double hz=target+std::remainder(phase-previous,2*pi)*sr/(2*pi*hop);errors.push_back(std::abs(1200*std::log2(hz/target)));}
            previous=phase;have=true;
        }
        for(int i=static_cast<int>(sr*.3)+1;i<static_cast<int>(audio.size());++i) {
            assert(std::isfinite(audio[static_cast<size_t>(i)]));
            peak=std::max(peak,std::abs(static_cast<double>(audio[static_cast<size_t>(i)])));power+=audio[static_cast<size_t>(i)]*static_cast<double>(audio[static_cast<size_t>(i)]);
        }
        std::sort(errors.begin(),errors.end());
        std::printf("rate=%.0f low=%d formants=%d p95=%.8f p99=%.8f gain=%.8f peak=%.8f\n",sr,low,formants,errors[static_cast<size_t>(.95*(errors.size()-1))],errors[static_cast<size_t>(.99*(errors.size()-1))],10*std::log10(power/inputPower),peak);
        std::fflush(stdout);
        assert(errors[static_cast<size_t>(.95*(errors.size()-1))]<14.0);
        assert(errors[static_cast<size_t>(.99*(errors.size()-1))]<15.0);
        assert(inputPower>0 && power>0 && std::abs(10*std::log10(power/inputPower))<.3);
        assert(peak<.28);
    }
}
