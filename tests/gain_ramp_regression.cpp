#include "GainRamp.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
#ifdef NDEBUG
#error Active assertions required
#endif
static std::vector<float> render(double sr,int block){
    const int length=static_cast<int>(sr*.08),a=static_cast<int>(sr*.02),b=a+19,c=static_cast<int>(sr*.05);
    humtune::GainRamp r;r.prepare(sr,1);std::vector<float> x(length,.25f),y(length,-.125f);
    for(int at=0;at<length;){
        if(at==a)r.setTarget(.25118864f);if(at==b)r.setTarget(1.9952623f);if(at==c)r.setTarget(.5011872f);
        int n=std::min(block,length-at);for(int boundary:{a,b,c})if(boundary>at)n=std::min(n,boundary-at);
        // This is how the plugin refreshes the unchanged target each callback.
        const float target=at<a?1:at<b?.25118864f:at<c?1.9952623f:.5011872f;r.setTarget(target);
        float* channels[]={x.data()+at,y.data()+at};r.process(channels,2,n);at+=n;
    }
    for(int i=0;i<length;++i){assert(std::isfinite(x[i]));assert(y[i]==-.5f*x[i]);}
    return x;
}
int main(){
    for(double sr:{44100.,48000.,96000.}){
        const auto ref=render(sr,1);for(int block:{7,127,512})assert(ref==render(sr,block));
        const int start=static_cast<int>(sr*.02),ramp=static_cast<int>(std::ceil(sr*.005));
        double maximum=0;for(size_t i=1;i<ref.size();++i)maximum=std::max(maximum,static_cast<double>(std::abs(ref[i]-ref[i-1])));
        assert(maximum<.25*2.0/ramp+1e-6);assert(ref[start-1]==.25f);
        humtune::GainRamp g;g.prepare(sr,1);g.setTarget(.25118864f);float before=.25f;double click=0;
        for(int i=0;i<ramp;++i){const float value=.25f*g.next();click=std::max(click,static_cast<double>(std::abs(value-before)));before=value;}
        assert(before==.25f*.25118864f);assert(g.next()==.25118864f);
        const double old=.25*(1-.25118864);assert(click<old/100);
        std::printf("sr=%.0f ramp=%d old_dc_step=%.9f new_max_dc_step=%.9f reduction=%.1fx\n",sr,ramp,old,click,old/click);
        g.prepare(sr,.5011872f);assert(g.next()==.5011872f);
        g.setTarget(std::numeric_limits<float>::quiet_NaN());for(int i=0;i<ramp;++i)assert(std::isfinite(g.next()));assert(g.next()==1);
        float x[3]={.25f,-.5f,1},*channels[]={x};g.prepare(sr,1);g.process(channels,1,3);assert(x[0]==.25f&&x[1]==-.5f&&x[2]==1);
    }
}
