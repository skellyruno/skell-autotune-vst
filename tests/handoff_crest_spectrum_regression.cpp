#include <PitchShifter.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi = 3.1415926535897932384626433832795;
constexpr int size = 32768;
void fft(std::vector<std::complex<double>>& x) {
    for (int i=1,j=0;i<size;++i) {
        int bit=size/2;for(;j&bit;bit/=2)j^=bit;j^=bit;
        if(i<j)std::swap(x[i],x[j]);
    }
    for(int length=2;length<=size;length*=2) {
        const auto root=std::polar(1.0,-2*pi/length);
        for(int base=0;base<size;base+=length) {
            std::complex<double> phase=1;
            for(int i=0;i<length/2;++i) {
                const auto a=x[base+i],b=x[base+i+length/2]*phase;
                x[base+i]=a+b;x[base+i+length/2]=a-b;phase*=root;
            }
        }
    }
}
}
namespace humtune {
struct PitchShifterSpectralOracle {
    static void run(double rate,double phase,int sourceBin,int targetBin) {
        constexpr float amplitude=.97f;
        const double sourceHz=rate*sourceBin/size,targetHz=rate*targetBin/size;
        PitchShifter p;p.prepare(rate,1);p.setLowLatencyExperiment(true);
        p.setLowLatencyBudgetMilliseconds(1.9);p.setEnergyMatchedHandoffEnabled(true);
        PitchShifter::PhaseMarkLink link;
        p.setPhaseMarkLink(&link,false);
        // Pure sine carrier plus a different pure sine dry stream. Neither
        // contains energy above 1 kHz; any persistent upper band is introduced
        // by the handoff. Freeze model acquisition to isolate the mixer.
        p.liveSpectrumValid=true;p.livePhaseInitialized=true;p.livePhase=phase;
        p.liveFrequency=targetHz;p.liveCurrentCount=p.liveOldCount=1;
        p.liveWaveSize.fill(0);p.liveSpectrumBlend=1;
        p.liveCoefficients(p.liveCurrentIndex)[1]=amplitude*.5f;
        p.liveCoefficients(p.liveOldIndex)[1]=amplitude*.5f;
        p.recordedPeriod=rate/sourceHz;p.dryMix=p.dryMixTarget=.5f;
        std::vector<std::complex<double>> spectrum(size);
        double peak=0;
        for(int i=0;i<2*size;++i) {
            const float input=amplitude*static_cast<float>(std::cos(2*pi*sourceBin*i/size));
            const float out=p.processLiveSpectrumSample(input,rate/sourceHz,targetHz/sourceHz);
            assert(std::isfinite(out));
            if(i>=size) {spectrum[i-size]=out;peak=std::max(peak,double(std::abs(out)));}
        }
        fft(spectrum);
        double total=0,upper=0;
        for(int bin=1;bin<size/2;++bin) {
            const double energy=std::norm(spectrum[bin]);total+=energy;
            if(double(bin)*rate/size>=3000)upper+=energy;
        }
        const double fraction=upper/std::max(1e-30,total);
        std::printf("rate=%.0f phase=%.3f source=%.4f target=%.4f peak=%.9f upper_fraction=%.12g\n",rate,phase,sourceHz,targetHz,peak,fraction);
#ifndef SKELLYTUNE_MEASURE_HANDOFF_ONLY
        assert(peak<=double(amplitude)+3e-6);
        assert(fraction<1e-5);
#endif
    }
};
}
int main() {
    for(double rate:{44100.,48000.,96000.}) {
        const int source=static_cast<int>(std::lround(200.0*size/rate));
        const int target=static_cast<int>(std::lround(225.0*size/rate));
        for(double phase:{0.,.125,.375,.625})
            for(auto bins:{std::array<int,2>{source,target},std::array<int,2>{target,source}})
                humtune::PitchShifterSpectralOracle::run(rate,phase,bins[0],bins[1]);
    }
}
