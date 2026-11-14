#include "PitchShifter.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cassert>
#ifdef NDEBUG
#error "Release regression assertions must stay active"
#endif
constexpr double pi=3.141592653589793;
double envelope(double f) { return .45*std::exp(-.5*std::pow((f-850)/160,2))+.25*std::exp(-.5*std::pow((f-2300)/240,2))+.04*std::exp(-f/2400); }
int main(){
for(bool low:{false,true}) for(bool preserve:{false,true}) for(float color:{-4.f,0.f,4.f}){
 humtune::PitchShifter s;s.prepare(48000,127);s.setMinFrequency(80);s.setLowLatencyExperiment(low);s.setLowLatencyBudgetMilliseconds(1.9);s.setPhaseLockedSource(true);s.setFormantPreserve(preserve);s.setFormantShiftSemitones(color);
 std::vector<float> x(48000);for(int i=0;i<48000;++i) for(int h=1;h<=60;++h) x[i]+=float(.12*envelope(h*180)*sin(2*pi*h*180*i/48000+.11*h*h));
 double inputPower=0;for(int i=24000;i<48000;++i) inputPower+=x[i]*x[i];
 for(int i=0;i<48000;i+=127)s.process(x.data()+i,std::min(127,48000-i),48000.f/180,240.f/180);
 double power=0,num=0,den=0,dot=0,aa=0,bb=0;for(int i=24000;i<48000;++i)power+=x[i]*x[i];
 for(int h=1;h<=40;++h){double re=0,im=0;for(int i=24000;i<48000;++i){double w=.5-.5*cos(2*pi*(i-24000)/24000);re+=w*x[i]*cos(2*pi*h*240*i/48000);im-=w*x[i]*sin(2*pi*h*240*i/48000);}double a=sqrt(re*re+im*im)/6000;double hz=h*240;double expected=envelope(hz/exp2(color/12.));dot+=a*expected;aa+=a*a;bb+=expected*expected;if(hz>=480&&hz<=1680){num+=hz*a*a;den+=a*a;}}
 const double cosine=dot/sqrt(aa*bb),gain=10*log10(power/inputPower);
 assert(std::isfinite(cosine) && std::isfinite(gain));
 assert(std::abs(gain)<1.0);
 if(preserve) assert(cosine>0.97);
 else assert(cosine<0.75);
 printf("preserve %d color %.0f envelopeCosine %.5f F1centroid %.1f gainDb %.2f\n",preserve,color,dot/sqrt(aa*bb),num/den,10*log10(power/inputPower));
}}
