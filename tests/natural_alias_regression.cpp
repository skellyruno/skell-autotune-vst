#include "AutoTuneEngine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef NDEBUG
#error "Natural alias regression requires active assertions"
#endif
struct Result {std::vector<float> audio;int bad=0,correct=0;};
static Result render(const std::vector<float>& raw,double sr,bool low,int block,int phrase) {
    humtune::AutoTuneEngine engine;
    engine.setRangeProtection(phrase==2);engine.setVoiceRange(phrase==2 ? 4 : 2);
    engine.setRootNote(phrase==2 ? 7 : 9);engine.setScaleType(0);engine.setAmount(1);
    engine.setRetuneSpeed(0);engine.setHumanize(0);engine.setNoteStabilizer(false);
    engine.setFormantPreserve(true);engine.setLowLatencyMode(low);engine.prepare(sr,block);
    struct Context {double sr;int phrase;Result* result;};Result result;Context context{sr,phrase,&result};
    engine.setOfflineTraceCallback([](void* pointer,const humtune::AutoTuneEngine::HopTrace& trace) noexcept {
        auto& c=*static_cast<Context*>(pointer);const double time=trace.sample/c.sr;
        const double begin=c.phrase==0 ? .22 : (c.phrase==1 ? .35 : .445);
        const double end=c.phrase==0 ? .27 : (c.phrase==1 ? .40 : .485);
        if(time>begin && time<end) {
            // The alias phrase retains its lower fundamental; the other two
            // fixed phrases guard against lower-octave invention and holding
            // back a real register change. No candidate-dependent selection.
            const double hz=trace.rawObservedHz;
            const bool right=hz>180 && hz<280;
            c.result->bad+=!right;c.result->correct+=right;
        }
    },&context);
    const int frames=static_cast<int>(raw.size()/2*sr/44100);result.audio.resize(static_cast<size_t>(frames)*2);
    for(int at=0;at<frames;at+=block) {
        const int count=std::min(block,frames-at);juce::AudioBuffer<float> buffer(2,count);
        for(int i=0;i<count;++i)for(int ch=0;ch<2;++ch) {
            const double position=(at+i)*44100./sr;const int j=static_cast<int>(position);const double fraction=position-j;
            buffer.getWritePointer(ch)[i]=static_cast<float>(raw[static_cast<size_t>(2*j+ch)]*(1-fraction)+raw[static_cast<size_t>(2*std::min(j+1,static_cast<int>(raw.size()/2)-1)+ch)]*fraction);
        }
        engine.process(buffer);
        for(int ch=0;ch<2;++ch)for(int i=0;i<count;++i) {
            const float value=buffer.getReadPointer(ch)[i];assert(std::isfinite(value));
            result.audio[static_cast<size_t>(2*(at+i)+ch)]=value;
        }
    }
    return result;
}
int main() {
    const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures";
    const char* names[]={"natural_a_alias.f32","natural_a_register_guard.f32","natural_g_register_guard.f32"};
    for(int phrase=0;phrase<3;++phrase) {
        std::ifstream input(fixtures/names[phrase],std::ios::binary|std::ios::ate);assert(input);
        std::vector<float> raw(static_cast<size_t>(input.tellg())/sizeof(float));input.seekg(0);
        input.read(reinterpret_cast<char*>(raw.data()),raw.size()*sizeof(float));assert(raw.size()==44000);
        for(double sr:{44100.,48000.,96000.})for(bool low:{false,true}) {
            const auto reference=render(raw,sr,low,127,phrase);
            std::printf("phrase=%d rate=%.0f low=%d false=%d correct=%d\n",phrase,sr,low,reference.bad,reference.correct);std::fflush(stdout);
            assert(reference.correct>=8 && reference.bad==0);
            for(int block:{1,512})assert(reference.audio==render(raw,sr,low,block,phrase).audio);
        }
    }
}
