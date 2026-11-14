#include <EpochObservation.h>
#include <PitchDetector.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef NDEBUG
#error "Epoch observation regression requires active assertions"
#endif
int main() {
    for(double rate:{44100.,48000.,96000.}) {
        humtune::EpochObservation hint;
        assert(hint.periodAt(0,rate/240,rate)==rate/240);
        const auto frequency=[](double time){return 240*std::exp2(800*time/1200.);};
        hint.observe(frequency(.10),.015,1.,.115*rate,rate);
        hint.observe(frequency(.104),.015,1.,.119*rate,rate);
        for(double time:{.095,.10,.104,.11,.12})assert(std::abs(1200*std::log2(rate/hint.periodAt(time*rate,rate/240,rate)/frequency(time)))<1e-8);
        assert(hint.periodAt(.2*rate,rate/240,rate)==0);
        hint.observe(480,.015,.3,.123*rate,rate);assert(hint.periodAt(.11*rate,rate/240,rate)==0);
        hint.reset();hint.observe(240,.015,1.,.115*rate,rate);hint.observe(480,.015,1.,.119*rate,rate);
        assert(hint.periodAt(.104*rate,rate/240,rate)==0); // A discrete leap cannot seed a velocity forecast.
        hint.observe(480,.015,1.,.123*rate,rate);assert(std::abs(hint.periodAt(.108*rate,0,rate)-rate/480)<1e-10);

        humtune::PitchDetector detector;detector.prepare(rate,127);detector.setMinFrequency(80);
        const int hop=detector.getHopSize(),dec=static_cast<int>(std::lround(rate/12000));
        std::vector<float> cycle(static_cast<size_t>(hop));
        for(int i=0;i<hop;++i)cycle[static_cast<size_t>(i)]=static_cast<float>(.2*std::sin(6.283185307179586*i/hop));
        float hz=0;
        for(int i=0;i<100;++i){detector.feedSamples(cycle.data(),hop);hz=detector.detectPitch();}
        assert(hz>0 && std::abs(hz-rate/hop)<1);
        const double analysisRate=rate/dec;
        const int window=static_cast<int>(std::ceil(analysisRate/80));
        const double newest=.5*(window-1+analysisRate/hz)/analysisRate;
        const double tagged=detector.getTaggedPitchEstimateAgeSeconds(hz),legacy=detector.getPitchEstimateAgeSeconds(hz);
        assert(std::abs(tagged-newest-15/rate)<1e-7);
        assert(std::abs(legacy-newest-static_cast<double>(hop)/rate)<1e-7);
        for(int i=0;i<dec+3;++i) {
            detector.feedSamples(cycle.data()+i,1);assert(detector.detectPitch()==hz);
            assert(std::abs(detector.getTaggedPitchEstimateAgeSeconds(hz)-tagged-(i+1)/rate)<1e-10);
            assert(detector.getPitchEstimateAgeSeconds(hz)==legacy);
        }
        detector.setMinFrequency(100);assert(detector.detectPitch()==0 && detector.getTaggedPitchEstimateAgeSeconds(0)==0);
        std::printf("rate=%.0f historical observation interpolation, leap/dropout rejection, tagged FIR/median age and legacy compatibility PASS\n",rate);
    }
}
