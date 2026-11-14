#include "PitchShifter.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#ifdef NDEBUG
#error "Periodic basis oracle requires active assertions"
#endif
namespace humtune {
struct PitchShifterSpectralOracle {
    static void checkControlCache() {
        PitchShifter shifter;
        for (double rate : {44100.0, 96000.0, 48000.0}) {
            shifter.prepare(rate, 127);
            for (double amount : {1.0, 0.0, 0.5, 0.000001, 0.999999, 1.0}) {
                shifter.smoothedFormantPreserve = amount;
                for (double ratio : {1.0, 0.6, 1.8, 0.93, 0.93, 1.0}) {
                    const double expected = amount == 1.0 ? 1.0 : amount == 0.0 ? ratio
                        : std::exp((1.0 - amount) * std::log(ratio));
                    assert(shifter.standardPreserveFactor(ratio) == expected);
                }
                const double target = shifter.formantPreserve ? 1.0 : 0.0;
                double expected = target + (amount - target) * std::exp(-32.0 / (rate * 0.012));
                if (std::abs(expected - target) < 1e-6) expected = target;
                shifter.smoothFormantPreserve();
                assert(shifter.smoothedFormantPreserve == expected);
            }
            shifter.reset();
            assert(shifter.standardPreserveFactor(1.13) == 1.0);
        }
    }
    static double check(double sr,double hz,double ratio,bool formants,double colour) {
        PitchShifter fast,reference;
        for(auto* p:{&fast,&reference}) {
            p->prepare(sr,127);p->setMinFrequency(35);p->setFormantPreserve(formants);p->reset();
            p->inCount=2*p->inSize+137;
            for(int64_t n=p->inCount-p->inSize;n<p->inCount;++n) {
                const double u=(n-p->inCount)/sr,phase=6.283185307179586*hz*u;
                p->inRing[static_cast<size_t>(n&p->inMask)]=static_cast<float>(
                    .18*std::sin(phase)+.08*std::sin(3*phase+.31)+.03*std::sin(11*phase+.7)+.007*std::sin(phase*1.17));
            }
        }
        reference.setDetailedDiagnosticsEnabled(true);
        const double period=sr/hz,hop=period/ratio,rate=(formants?1.:ratio)*std::exp2(colour/12.);
        const double epoch=fast.inCount-2*period-37.123;
        // Audit reciprocal wrapping against the original division-based reader,
        // including either side of the cycle seam and its exact endpoints.
        const double fade = std::min({0.08 * period, 0.00045 * sr,
                                      std::max(0.0, epoch - period - 2.0)});
        for (int cycle = -8; cycle <= 8; ++cycle) {
            for (double fraction : {0.0, 1e-12, 0.13, 0.72, 1.0 - 1e-12}) {
                const double position = epoch + (cycle + fraction) * period;
                double wrapped = position;
                wrapped -= std::floor((wrapped - (epoch - period)) / period) * period;
                float expected = reference.readKnownSource(wrapped);
                if (fade > 2.0 && wrapped > epoch - fade) {
                    const float previous = reference.readKnownSource(wrapped - period);
                    const double u = (wrapped - (epoch - fade)) / fade;
                    expected += static_cast<float>((previous - expected) * u * u * (3.0 - 2.0 * u));
                }
                const float actual = fast.readCompleteCycle(position, epoch, period, 1.0 / period, fade);
                assert(std::abs(actual - expected) < 2e-6f);
            }
        }
        double maximum=0;
        for(int update=0;update<3;++update) {
            fast.buildLiveSpectrum(epoch+update*period*.173,period,hop,rate);
            reference.buildLiveSpectrum(epoch+update*period*.173,period,hop,rate);
            for(double blend:{0.,.37,1.}) {
                fast.liveSpectrumBlend=reference.liveSpectrumBlend=blend;
                for(int i=0;i<97;++i) {
                    const double a=fast.evaluateLiveSpectrum(i/97.,sr/hop),b=reference.evaluateLiveSpectrum(i/97.,sr/hop);
                    assert(std::isfinite(a)&&std::isfinite(b));maximum=std::max(maximum,std::abs(a-b));
                }
            }
            fast.liveSpectrumBlend=reference.liveSpectrumBlend=.37;
        }
        assert(maximum<2e-6);return maximum;
    }
};
}
int main(){humtune::PitchShifterSpectralOracle::checkControlCache();int cases=0;double maximum=0;
    for(double sr:{44100.,48000.,96000.})for(double hz:{40.,80.,220.,880.})
        for(double ratio:{.6,.83,1.,1.13,1.8})for(bool formants:{false,true})for(double colour:{0.,2.}) {
            maximum=std::max(maximum,humtune::PitchShifterSpectralOracle::check(sr,hz,ratio,formants,colour));++cases;
        }
    std::printf("periodic basis oracle cases=%d maximum waveform delta=%.9g\n",cases,maximum);
}
