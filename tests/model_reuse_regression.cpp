#include <PitchShifter.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <array>
#ifdef NDEBUG
#error "Model reuse needs active regression assertions"
#endif
namespace humtune {
struct PitchShifterSpectralOracle {
 static int check(double sr,double sourceHz,bool preserve,bool low){
  PitchShifter p;p.prepare(sr,127);p.setMinFrequency(35);p.setLowLatencyExperiment(low);p.setFormantPreserve(preserve);p.setPhaseLockedSource(true);
  assert(p.inSize>=8192);p.inCount=8192;
  const double period=sr/sourceHz,epoch=6000,targetHz=sourceHz*std::exp2(1./12),hop=sr/targetHz;
  const auto rateFor=[&](double h){return preserve?1.:period/h;};
  for(int i=0;i<8192;++i){const double phase=6.283185307179586*i/period;p.inRing[static_cast<size_t>(i&p.inMask)]=static_cast<float>(.16*std::sin(phase)+.06*std::sin(3*phase+.2)+.03*std::sin(11*phase+.5));}
  p.liveWaveClockValid=true;p.liveWaveLastWantedHz=targetHz;p.liveWaveStableSince=p.inCount-static_cast<int64_t>(.02*sr);
  p.buildLiveSpectrum(epoch,period,hop,rateFor(hop));p.liveSpectrumBlend=1;
  std::array<float,64> reference{};for(int i=0;i<64;++i)reference[i]=p.evaluateLiveSpectrum(i/64.,targetHz);
  int eligible=0,redundant=0;double maxDifference=0;
  for(int k=0;k<20;++k){
   const double nextHop=hop*(1.+(k&1?1.:-1.)*7e-8),nextHz=sr/nextHop;
   const bool cached=p.liveWaveAcceptsFrequency(p.liveCurrentIndex,nextHz);const int index=p.liveCurrentIndex;
   p.buildLiveSpectrum(epoch,period,nextHop,rateFor(nextHop));p.liveSpectrumBlend=1;
   eligible+=cached;redundant+=cached && p.liveCurrentIndex!=index;
   for(int i=0;i<64;++i){const float sample=p.evaluateLiveSpectrum(i/64.,nextHz);assert(std::isfinite(sample));maxDifference=std::max(maxDifference,std::abs(double(sample)-reference[i]));}
  }
  assert(eligible>0 && maxDifference<1e-4);
  // A stream of individually tiny moves must not drift arbitrarily far from
  // the actual cached model. A scheduled admission is not a new model key.
  double lastActualHop=hop;
  for(int k=1;k<=12;++k){
   const double nextHop=hop*(1.+k*7e-8);const int index=p.liveCurrentIndex;
   p.buildLiveSpectrum(epoch,period,nextHop,rateFor(nextHop));p.liveSpectrumBlend=1;
   if(p.liveCurrentIndex!=index)lastActualHop=nextHop;
   assert(std::abs(nextHop-lastActualHop)<=2.01e-7*lastActualHop);
  }
  const auto mustBuild=[&](double e,double per,double h,double rate,double forced=std::numeric_limits<double>::quiet_NaN()){
   p.liveSpectrumBlend=1;const int index=p.liveCurrentIndex;p.buildLiveSpectrum(e,per,h,rate,forced);assert(p.liveCurrentIndex!=index);p.liveSpectrumBlend=1;
  };
  mustBuild(epoch,period,hop*1.01,rateFor(hop*1.01));
  mustBuild(epoch+17,period,hop,rateFor(hop));
  mustBuild(epoch+17,period*1.01,hop,rateFor(hop));
  mustBuild(epoch+17,period*1.01,hop,rateFor(hop)*1.01);
  mustBuild(epoch+17,period*1.01,hop,rateFor(hop)*1.01,p.livePreviousRotation+.1);
  p.reset();assert(!p.liveSpectrumValid);
  std::printf("model reuse sr=%.0f source=%.0f preserve=%d low=%d eligible=%d redundant=%d maxShapeDifference=%.9g\n",sr,sourceHz,preserve,low,eligible,redundant,maxDifference);std::fflush(stdout);
  return redundant;
 }
};
}
int main(){int redundant=0;for(double sr:{44100.,48000.,96000.})for(double hz:{40.,220.,880.})for(bool preserve:{false,true})for(bool low:{false,true})redundant+=humtune::PitchShifterSpectralOracle::check(sr,hz,preserve,low);return redundant?1:0;}
