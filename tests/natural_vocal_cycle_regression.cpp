#include "AutoTuneEngine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#ifdef NDEBUG
#error "Natural vocal precision gates require active assertions"
#endif
#include <sstream>
#include <string>
#include <vector>
struct Frame {int sample,midi;};
static double correlation(const std::vector<float>& audio,int center,double lag) {
    constexpr int half=768;double xy=0,xx=0,yy=0;
    for(int k=-half;k<half;++k) {
        const int i=center+k;const double position=i-lag;const int j=static_cast<int>(position);const double fraction=position-j;
        if(j<0 || i>=static_cast<int>(audio.size()) || j+1>=static_cast<int>(audio.size()))return -1;
        const double a=audio[static_cast<size_t>(i)],b=audio[static_cast<size_t>(j)]*(1-fraction)+audio[static_cast<size_t>(j+1)]*fraction;
        xy+=a*b;xx+=a*a;yy+=b*b;
    }
    return xy/std::sqrt(std::max(1e-30,xx*yy));
}
// A fixed real-vocal frame set tests complete-waveform periodicity. The local
// lag fit is conditioned on independent commercial-consensus target notes;
// it is not a general pitch detector or an octave-error metric. No frame is
// removed because of candidate output level, pitch or correlation.
int main() {
    const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures";
    std::ifstream input(fixtures/"near_dual_mono_4s.f32",std::ios::binary|std::ios::ate);assert(input);
    std::vector<float> raw(static_cast<size_t>(input.tellg())/sizeof(float));input.seekg(0);input.read(reinterpret_cast<char*>(raw.data()),raw.size()*sizeof(float));
    assert(raw.size()==4*44100*2);
    std::vector<Frame> frames;std::ifstream selected(fixtures/"natural_g_held_frames.csv");assert(selected);std::string line;std::getline(selected,line);
    while(std::getline(selected,line)){std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);Frame f{};row>>f.sample>>f.midi;assert(row);frames.push_back(f);}
    assert(frames.size()>100);
    for(bool low:{false,true})for(bool formants:{false,true}) {
        humtune::AutoTuneEngine e;e.setRangeProtection(true);e.setVoiceRange(4);e.setRootNote(7);e.setScaleType(0);e.setRetuneSpeed(0);e.setAmount(1);e.setHumanize(0);e.setNoteStabilizer(false);e.setFormantPreserve(formants);e.setLowLatencyMode(low);e.prepare(44100,127);
        std::vector<float> mono(raw.size()/2);
        for(int at=0;at<static_cast<int>(mono.size());at+=127) {
            const int n=std::min(127,static_cast<int>(mono.size())-at);juce::AudioBuffer<float> buffer(2,n);
            for(int ch=0;ch<2;++ch)for(int i=0;i<n;++i)buffer.getWritePointer(ch)[i]=raw[static_cast<size_t>(2*(at+i)+ch)];
            e.process(buffer);for(int i=0;i<n;++i)mono[static_cast<size_t>(at+i)]=.5f*(buffer.getReadPointer(0)[i]+buffer.getReadPointer(1)[i]);
        }
        std::vector<double> errors;int weak=0;
        for(const auto& f:frames) {
            const double hz=440*std::exp2((f.midi-69)/12.),expected=44100/hz;
            const int center=f.sample+e.getLatencySamples(),begin=static_cast<int>(std::floor(expected*std::exp2(-.7/12))),end=static_cast<int>(std::ceil(expected*std::exp2(.7/12)));
            int best=begin;double maximum=-2;
            for(int lag=begin;lag<=end;++lag) {const double score=correlation(mono,center,lag);if(score>maximum){maximum=score;best=lag;}}
            const double a=correlation(mono,center,best-1),b=correlation(mono,center,best+1),curve=a-2*maximum+b;
            const double offset=curve<-1e-12 ? std::clamp(.5*(a-b)/curve,-.5,.5) : 0;
            errors.push_back(std::abs(1200*std::log2(expected/(best+offset))));weak+=maximum<.9;
        }
        std::sort(errors.begin(),errors.end());std::printf("low=%d formants=%d frames=%zu median=%.6f p95=%.6f p99=%.6f weak=%d\n",low,formants,errors.size(),errors[errors.size()/2],errors[static_cast<size_t>(.95*(errors.size()-1))],errors[static_cast<size_t>(.99*(errors.size()-1))],weak);
        std::fflush(stdout);
        assert(errors[errors.size()/2]<1.0);
        assert(errors[static_cast<size_t>(.95*(errors.size()-1))]<(low ? 30.0 : 10.0));
        assert(errors[static_cast<size_t>(.99*(errors.size()-1))]<55.0);
        assert(weak<=12);
    }
}
