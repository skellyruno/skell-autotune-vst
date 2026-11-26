#include "../Source/MetaTune25Engine.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
static void need(bool ok,const char* m){if(!ok){std::cerr<<m<<'\n';std::exit(1);}}
int main(){
 for(int fs:{44100,48000,96000})for(bool low:{true,false}){
  std::array<bool,12> mask{};mask[9]=true;
  humtune::MetaTune25Engine e;e.setCustomScale(mask);e.setLowLatencyMode(low);e.prepare(fs,127);
  autotune::SkellyFusionEngine native;native.prepare(fs,70.f,1000.f);
  autotune::Settings p;p.key=0;p.scale=autotune::Scale::Custom;p.customScale=mask;p.retuneMs=0;
  p.lowLatency=low;p.ultraLowLatency=low;p.strictRealtime=true;p.snapLive=false;p.noteConfirmMs=10.;
  p.noteStabilizerMs=0.;p.follow=.85f;p.voicingThreshold=.68f;native.setSettings(p);native.reset();
  const int d=e.getLatencySamples();need(d==native.latencySamples(),"native delay mismatch");
  const int n=fs*2;std::vector<float> l(n),r(n);for(int i=0;i<n;++i){double ph=6.283185307179586*225*i/fs;l[i]=float(.18*std::sin(ph)+.04*std::sin(2*ph));r[i]=.7f*l[i];}
  auto x=l,y=r;
  for(int i=0;i<n;i+=128){float* a[]={l.data()+i,r.data()+i};float* b[]={x.data()+i,y.data()+i};int k=std::min(128,n-i);e.process(a,2,k);native.processChannels(b,2,k);}
  for(int i=fs/2;i<n;++i)need(l[i]==x[i]&&r[i]==y[i],"neutral integration differs from native MetaTune 2.5");
  need(e.getTargetMidiNote()==57,"native target missing");
  for(float st:{-4.f,4.f,0.f}){e.setFormantShiftSemitones(st);float b[128]{};float* io[]={b};e.process(io,1,128);need(e.getLatencySamples()==d,"formant altered native PDC");}
  humtune::MetaTune25Engine dry;dry.setLowLatencyMode(low);dry.setEnabled(false);dry.setFormantShiftSemitones(4.f);dry.prepare(fs,127);
  std::vector<float> impulse(fs/4);impulse[0]=.5f;
  for(int i=0;i<int(impulse.size());i+=127){float* io[]={impulse.data()+i};dry.process(io,1,std::min(127,int(impulse.size())-i));}
  for(int i=0;i<int(impulse.size());++i)need(impulse[i]==(i==dry.getLatencySamples()?.5f:0.f),"bypass PDC mismatch");
  std::cout<<fs<<" low="<<low<<" MetaTune native parity, target, no added formant delay, bypass PASS\n";
 }
}
