#include <AutoTuneEngine.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <vector>
#ifdef NDEBUG
#error "Harmonic pop regression requires active assertions"
#endif
namespace {
std::atomic<bool> tracking{false};
std::atomic<unsigned long long> allocations{0};
}
void* operator new(std::size_t bytes) {
    if(tracking.load(std::memory_order_relaxed))allocations.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
struct Result {std::vector<float> audio;int rawAliases=0,falseNotes=0,rejected=0,wrongTurn=0,turnHops=0;};
std::vector<float> fixture() {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"fixtures/moderate_harmonic_pop_6s.f32";
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    constexpr size_t floats=6*44100*2;
    assert(f.good() && f.tellg()==std::streampos(floats*sizeof(float)));
    f.seekg(0);std::vector<float> raw(floats);
    f.read(reinterpret_cast<char*>(raw.data()),static_cast<std::streamsize>(floats*sizeof(float)));
    assert(f.good());return raw;
}
Result render(const std::vector<float>& raw,int block,bool preserve) {
    humtune::AutoTuneEngine e;
    e.setVoiceRange(4);e.setRangeProtection(true);e.setRootNote(7);e.setScaleType(0);
    e.setAmount(1);e.setRetuneSpeed(0);e.setHumanize(0);e.setNoteStabilizer(false);
    e.setFormantPreserve(preserve);e.setLowLatencyMode(true);e.setEnabled(true);e.prepare(44100,block);
    Result result;result.audio.resize(raw.size());
    e.setOfflineTraceCallback([](void* ptr,const humtune::AutoTuneEngine::HopTrace& t) noexcept {
        auto& r=*static_cast<Result*>(ptr);
        const double time=t.sample/44100.;
        if(time>5.90 && time<5.907) {
            ++r.turnHops;
            r.wrongTurn+=t.selectedTargetMidi!=57;
        }
        if(time>5.81 && time<5.831) {
            r.rawAliases+=t.rawObservedHz>400;
            r.falseNotes+=t.selectedTargetMidi>=70;
            r.rejected+=t.rejectedUpperHarmonic;
        }
    },&result);
    for(int at=0;at<static_cast<int>(raw.size()/2);at+=block) {
        const int count=std::min(block,static_cast<int>(raw.size()/2)-at);
        juce::AudioBuffer<float> b(2,count);
        for(int ch=0;ch<2;++ch)for(int i=0;i<count;++i)b.getWritePointer(ch)[i]=raw[2*(at+i)+ch];
        tracking.store(true,std::memory_order_relaxed);e.process(b);tracking.store(false,std::memory_order_relaxed);
        for(int ch=0;ch<2;++ch)for(int i=0;i<count;++i)result.audio[2*(at+i)+ch]=b.getReadPointer(ch)[i];
    }
    return result;
}
void trueRegisterJump(double sr,bool low,bool preserve,double multiple) {
    humtune::AutoTuneEngine e;
    e.setVoiceRange(4);e.setRangeProtection(true);e.setScaleType(2);
    e.setRetuneSpeed(0);e.setNoteStabilizer(false);e.setFormantPreserve(preserve);
    e.setLowLatencyMode(low);e.prepare(sr,127);
    struct Context {double sr,target,first=-1;int consecutive=0;};
    Context context{sr,240*multiple};
    e.setOfflineTraceCallback([](void* ptr,const humtune::AutoTuneEngine::HopTrace& t) noexcept {
        auto& c=*static_cast<Context*>(ptr);const double time=t.sample/c.sr;
        if(time<.3)return;
        if(std::abs(t.observedHz/c.target-1)<.02) {
            ++c.consecutive;if(c.consecutive==2 && c.first<0)c.first=time-.3;
        } else c.consecutive=0;
    },&context);
    double phase=0;const int total=static_cast<int>(.55*sr);
    for(int at=0;at<total;at+=127) {
        const int count=std::min(127,total-at);juce::AudioBuffer<float> b(1,count);
        for(int i=0;i<count;++i) {
            const double hz=(at+i)<.3*sr ? 240 : 240*multiple;
            phase+=6.283185307179586*hz/sr;
            b.getWritePointer(0)[i]=static_cast<float>(.18*std::sin(phase)+.06*std::sin(2*phase+.3)+.025*std::sin(3*phase+.4));
        }
        tracking.store(true,std::memory_order_relaxed);e.process(b);tracking.store(false,std::memory_order_relaxed);
    }
    std::printf("genuine register rate=%.0f low=%d formants=%d multiple=%.0f stable_accept=%.3f ms\n",sr,low,preserve,multiple,1000*context.first);
    std::fflush(stdout);
    assert(context.first>=0 && context.first<.035);
}
}
int main() {
    const auto raw=fixture();
    for(bool preserve:{false,true}) {
        const auto a=render(raw,127,preserve);
        std::printf("real pop formants=%d raw_aliases=%d false_notes=%d rejected=%d\n",preserve,a.rawAliases,a.falseNotes,a.rejected);std::fflush(stdout);
        // Every alias the detector produces must be rejected; the floor only guarantees the fixture still
        // exercises the guard (the shorter Low Latency window aliases less often on this clip).
        assert(a.rawAliases>=2 && a.falseNotes==0 && a.rejected>=a.rawAliases);
        assert(a.turnHops>=2 && a.wrongTurn==0);
        for(int block:{1,512})assert(a.audio==render(raw,block,preserve).audio);
        double energy=0,peak=0;
        for(float sample:a.audio) {assert(std::isfinite(sample));energy+=static_cast<double>(sample)*sample;peak=std::max(peak,static_cast<double>(std::abs(sample)));}
        const double rms=std::sqrt(energy/a.audio.size());assert(rms>.03 && rms<.3 && peak<1.05);
    }
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})for(bool preserve:{false,true})
        for(double multiple:{2.,3.,4.})trueRegisterJump(sr,low,preserve,multiple);
    assert(allocations.load(std::memory_order_relaxed)==0);
}
