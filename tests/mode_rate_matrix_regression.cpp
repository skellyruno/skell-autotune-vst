#include "AutoTuneEngine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>
#include <iostream>
#ifdef NDEBUG
#error Assertions required
#endif
static std::vector<float> render(double sr,int block,bool low,bool stereo){
    humtune::AutoTuneEngine e;e.setVoiceRange(2);e.setRootNote(9);e.setRetuneSpeed(0);
    e.setFormantPreserve(false);e.setLowLatencyMode(low);e.prepare(sr,block);
    const int n=static_cast<int>(sr*.7),edge1=static_cast<int>(sr*.21),edge2=static_cast<int>(sr*.43);
    std::vector<float> result(static_cast<size_t>(2*n));double phase=0;
    for(int pos=0;pos<n;){
        if(pos==edge1)e.setLowLatencyMode(!low);
        if(pos==edge2){e.setLowLatencyMode(low);e.setVoiceRange(1);}
        const int next=pos<edge1?edge1:(pos<edge2?edge2:n);
        const int count=std::min(block,next-pos);juce::AudioBuffer<float>b(2,count);
        for(int i=0;i<count;++i){
            const double time=(pos+i)/sr;
            const double hz=time<.3?207.0:247.0;
            phase+=6.283185307179586*hz/sr;
            const float x=static_cast<float>(.18*std::sin(phase)+.07*std::sin(2*phase+.3));
            b.getWritePointer(0)[i]=x;
            b.getWritePointer(1)[i]=stereo ? .65f*x+static_cast<float>(.04*std::sin(6.283185307179586*331*time)) : x*(1.0f+1e-7f);
        }
        e.process(b);
        for(int i=0;i<count;++i)for(int ch=0;ch<2;++ch){float x=b.getReadPointer(ch)[i];assert(std::isfinite(x));assert(std::abs(x)<1);result[static_cast<size_t>(2*(pos+i)+ch)]=x;}
        pos+=count;
    }
    assert(e.getLatencySamples()==(low?static_cast<int>(std::ceil(sr*.0019)):static_cast<int>(std::ceil(1.3*std::ceil(sr/80)))+8));
    double difference=0;for(int i=n/2;i<n;++i)difference+=std::abs(result[static_cast<size_t>(2*i)]-result[static_cast<size_t>(2*i+1)]);
    assert(stereo ? difference/n>.001 : difference/n<.00001);
    return result;
}
int main(){for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})for(bool stereo:{false,true}){
    const auto a=render(sr,1,low,stereo);
    for(int block:{127,512}){const auto b=render(sr,block,low,stereo);assert(a==b);}
    std::cout<<"matrix sr="<<sr<<" low="<<low<<" trueStereo="<<stereo<<" passed\n";
}}
