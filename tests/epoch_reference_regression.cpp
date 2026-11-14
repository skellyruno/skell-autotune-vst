#include "PitchShifter.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#ifdef NDEBUG
#error "Correlation regression assertions must stay enabled"
#endif
namespace humtune {
struct PitchShifterSpectralOracle {
    static int run(std::FILE* output) {
        int cases=0;
        for(double rate:{8000.,44100.,48000.,96000.,192000.}) {
            PitchShifter p;p.prepare(rate,127);
            for(bool low:{false,true})for(bool live:{false,true}) {
                p.setLowLatencyExperiment(low);p.setPhaseLockedSource(live);
                for(int generation=0;generation<3;++generation) {
                    p.inCount=static_cast<int64_t>(p.inSize)*(generation+1)+173;
                    uint32_t random=0x1267ab89u+static_cast<uint32_t>(generation);
                    for(int64_t n=p.inCount-p.inSize;n<p.inCount;++n) {
                        random^=random<<13;random^=random>>17;random^=random<<5;
                        const double phase=6.283185307179586*117.3*n/rate;
                        const double noise=(static_cast<double>(random)/4294967296.-.5)*.025;
                        p.inRing[static_cast<size_t>(n&p.inMask)]=generation==2 ? 0.f :
                            static_cast<float>(.2*std::sin(phase)+.08*std::sin(3*phase+.7)+noise);
                    }
                    for(double hz:{35.,80.,110.,220.,440.,1500.}) {
                        const double period=rate/hz;
                        for(int probe=0;probe<16;++probe) {
                            // Fractional marks, long and short histories, ring wrap,
                            // and too-old / too-new recorded-history guards.
                            double expected=p.inCount-3-period*(probe%4+.2);
                            double previous=expected-period+(probe%3-1)*.375;
                            if(probe==14)previous=p.inCount-p.inSize+1;
                            if(probe==15)expected=p.inCount+2;
                            const double actual=p.correlateSourceEpoch(expected,previous,period);
                            const int limit=std::clamp(static_cast<int>(.08*period),2,live?p.epochRadiusLimit:48);
                            assert(std::isfinite(actual));
                            assert(std::abs(actual-expected)<=limit+.500001);
                            assert(p.candidateEpochConfidence>=0 && p.candidateEpochConfidence<=1.000001f);
                            if(output) {
                                assert(std::fwrite(&actual,sizeof(actual),1,output)==1);
                                assert(std::fwrite(&p.candidateEpochConfidence,sizeof(float),1,output)==1);
                            }
                            ++cases;
                        }
                    }
                }
            }
        }
        return cases;
    }
};
}
int main(int argc,char** argv) {
    std::FILE* output=argc==2?std::fopen(argv[1],"wb"):nullptr;
    if(argc==2)assert(output);
    const int cases=humtune::PitchShifterSpectralOracle::run(output);
    if(output)assert(std::fclose(output)==0);
    std::printf("PASS epoch reference: %d searches, rate/mode/history changes and silence\n",cases);
}
