#include <AutoTuneEngine.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef NDEBUG
#error "Regression requires active assertions"
#endif
namespace {
struct Result {std::vector<float> audio;int hops=0,wrong=0,rejected=0,lateHops=0,lateWrong=0,tailHops=0,tailWrong=0;};
std::vector<float> fixture(){
 const auto path=std::filesystem::path(__FILE__).parent_path()/"fixtures/standard_register_tail_16s.f32";
 std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f && f.tellg()==std::streampos(16*44100*2*sizeof(float)));
 std::vector<float> x(16*44100*2);f.seekg(0);f.read(reinterpret_cast<char*>(x.data()),static_cast<std::streamsize>(x.size()*sizeof(float)));assert(f);return x;
}
Result render(const std::vector<float>& x,int block,bool form){
 humtune::AutoTuneEngine e;e.setRangeProtection(true);e.setVoiceRange(4);e.setRootNote(7);e.setScaleType(0);e.setAmount(1);e.setRetuneSpeed(0);e.setHumanize(0);e.setNoteStabilizer(false);e.setFormantPreserve(form);e.setLowLatencyMode(false);e.setEnabled(true);e.prepare(44100,block);
 Result r;r.audio.resize(x.size());e.setOfflineTraceCallback([](void* ptr,const humtune::AutoTuneEngine::HopTrace& h)noexcept{
  auto& r=*static_cast<Result*>(ptr);const double time=h.sample/44100.;
  // In the supplied phrase the current waveform still supports the lower
  // register. A three-hop YIN upper harmonic must not create MIDI71 here.
  // Replacing the octave with a current-period estimate would introduce an
  // early A3/B3/A3 bounce. The historical YIN contour remains above this
  // scale boundary through these three observations, so retain B3 here.
  if(time>5.815 && time<5.829){++r.hops;r.wrong+=h.selectedTargetMidi!=59;r.rejected+=h.rejectedUpperHarmonic;}
  // The second burst ends in a high-confidence YIN harmonic. Repair the
  // whole contour: four B3 hops, followed by the genuine A3 crossing.
  if(time>15.320 && time<15.338){++r.tailHops;r.tailWrong+=h.selectedTargetMidi!=(time<15.336?59:57);}
  // The genuine following downward crossing must still commit immediately.
  if(time>5.833 && time<5.84){++r.lateHops;r.lateWrong+=h.selectedTargetMidi!=57;}
 },&r);
 for(int at=0;at<16*44100;at+=block){const int count=std::min(block,16*44100-at);juce::AudioBuffer<float> b(2,count);for(int ch=0;ch<2;++ch)for(int i=0;i<count;++i)b.getWritePointer(ch)[i]=x[2*(at+i)+ch];e.process(b);for(int ch=0;ch<2;++ch)for(int i=0;i<count;++i)r.audio[2*(at+i)+ch]=b.getReadPointer(ch)[i];}
 return r;
}
}
int main(){
 const auto x=fixture();int failures=0;
 for(bool form:{false,true}){
  const auto a=render(x,127,form);assert(a.hops==3);
  std::printf("Standard formants=%d three-hop burst: wrong=%d rejected=%d\n",form,a.wrong,a.rejected);std::fflush(stdout);
  failures+=a.wrong!=0;failures+=a.rejected!=3;assert(a.tailHops==5);failures+=a.tailWrong!=0;std::printf("tail formants=%d wrong=%d\n",form,a.tailWrong);std::fflush(stdout);
  assert(a.lateHops==2 && a.lateWrong==0);
  for(int block:{1,512}){auto b=render(x,block,form);assert(a.audio==b.audio && a.hops==b.hops && a.wrong==b.wrong && a.rejected==b.rejected && a.lateHops==b.lateHops && a.lateWrong==b.lateWrong && a.tailHops==b.tailHops && a.tailWrong==b.tailWrong);}
  double sum=0,peak=0;for(float v:a.audio){assert(std::isfinite(v));sum+=double(v)*v;peak=std::max(peak,double(std::abs(v)));}const double rms=std::sqrt(sum/a.audio.size());assert(rms>.03 && rms<.3 && peak<1.05);
 }
 return failures?1:0;
}
