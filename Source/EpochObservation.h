#pragma once
#include <algorithm>
#include <cmath>
namespace humtune {
// Independent, timestamped pitch evidence used only to qualify an epoch match.
// It never drives the target oscillator, note decisions or reported latency.
class EpochObservation {
public:
    void reset() noexcept {external=seeded=valid=false;velocity=0;}
    void observe(double hz,double age,double confidence,double latest,double rate) noexcept {
        external=true;valid=false;
        if(!std::isfinite(hz) || hz<=0 || !std::isfinite(age) || age<0 || confidence<.9 || !std::isfinite(confidence)) {seeded=false;return;}
        const double time=latest-age*rate,midi=1200*std::log2(hz);
        velocity=0;valid=true;
        if(seeded) {
            const double step=time-when,change=midi-cents;
            if(step<rate*.0005 || std::abs(change)>175)valid=false;
            else velocity=std::clamp(change*rate/step,-6000.,6000.);
        }
        when=time;cents=midi;seeded=true;
    }
    double periodAt(double time,double fallback,double rate) const noexcept {
        if(!external)return fallback;
        if(!valid || std::abs(time-when)>rate*.045)return 0;
        const double motion=std::clamp(velocity*(time-when)/rate,-85.,85.);
        return rate/std::exp2((cents+motion)/1200.);
    }
private:
    bool external=false,seeded=false,valid=false;
    double when=0,cents=0,velocity=0;
};
}
