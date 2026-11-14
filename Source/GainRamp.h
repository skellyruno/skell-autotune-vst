#pragma once
#include <algorithm>
#include <cmath>

namespace humtune {
// Sample-counted, stereo-linked gain automation. No audio delay or allocation.
// Repeated target values do not restart a ramp at host block boundaries.
class GainRamp {
public:
    void prepare(double sampleRate,float initialGain) noexcept {
        length=std::max(1,static_cast<int>(std::ceil((std::isfinite(sampleRate)&&sampleRate>0?sampleRate:44100)*.005)));
        current=target=valid(initialGain);step=0;remaining=0;
    }
    void setTarget(float gain) noexcept {
        gain=valid(gain);if(gain==target)return;
        target=gain;remaining=length;step=(target-current)/length;
    }
    float next() noexcept {
        if(remaining>0){current+=step;if(--remaining==0)current=target;}
        return current;
    }
    void process(float* const* channels,int channelCount,int samples) noexcept {
        if(samples<=0||channelCount<=0)return;
        if(remaining==0&&current==1)return;
        const int ramp=std::min(samples,remaining);
        for(int i=0;i<ramp;++i){const float gain=next();for(int c=0;c<channelCount;++c)channels[c][i]*=gain;}
        if(ramp<samples&&current!=1)for(int c=0;c<channelCount;++c)for(int i=ramp;i<samples;++i)channels[c][i]*=current;
    }
private:
    static float valid(float gain) noexcept {return std::isfinite(gain)&&gain>=0?gain:1;}
    int length=1,remaining=0;float current=1,target=1,step=0;
};
} // namespace humtune
