#include "../Source/ClassicEngine.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
static void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    for(int rate:{44100,48000,96000}){
        std::array<bool,12> mask{};mask[9]=true;
        humtune::ClassicEngine a;a.setCustomScale(mask);a.prepare(rate,127);
        snap::Engine reference;reference.prepare(rate,2);
        snap::Params p;p.mode=0;p.latencyMs=2.5;p.mask=1u<<9;p.refHz=440.;p.retuneMs=0.;p.amount=1.;p.minHz=80.;p.humanize=0.;
        reference.setParams(p);reference.reset();
        const int delay=a.getLatencySamples();
        require(delay==reference.actualDelaySamples(),"Classic native delay mismatch");
        const int count=rate*2;std::vector<float> l(count),r(count);
        for(int i=0;i<count;++i){double ph=6.283185307179586*225.*i/rate;l[i]=float(.18*std::sin(ph)+.04*std::sin(2*ph));r[i]=-.7f*l[i];}
        auto x=l,y=r;
        for(int i=0;i<count;i+=127){float* io[]={l.data()+i,r.data()+i};float* other[]={x.data()+i,y.data()+i};const int n=std::min(127,count-i);a.process(io,2,n);reference.process(other,2,n);}
        for(int i=rate/4;i<count;++i)require(l[i]==x[i]&&r[i]==y[i],"Standalone neutral route differs from exact native Classic");
        require(a.getTargetMidiNote()==57,"Classic target telemetry");
        humtune::ClassicEngine dry;dry.setEnabled(false);dry.prepare(rate,127);
        std::vector<float> impulse(rate/4);impulse[0]=.5f;
        for(int i=0;i<int(impulse.size());i+=127){float* io[]={impulse.data()+i};dry.process(io,1,std::min(127,int(impulse.size())-i));}
        for(int i=0;i<int(impulse.size());++i)require(impulse[i]==(i==dry.getLatencySamples()?.5f:0.f),"Classic bypass PDC mismatch");
        a.setFormantShiftSemitones(-3.f);float buf[128]{};float* io[]={buf};a.process(io,1,128);
        require(a.getLatencySamples()==delay+1024,"Formant PDC mismatch");
        a.setFormantShiftSemitones(0.f);a.process(io,1,128);
        require(a.getLatencySamples()==delay,"Neutral formant PDC mismatch");
        a.setLowLatencyMode(false);a.process(io,1,128);
        require(a.getLatencySamples()>=int(std::lround(rate*.01)),"Stable profile latency mismatch");
        std::cout<<rate<<" Classic native parity, target, bypass and formant latency PASS\n";
    }
}
