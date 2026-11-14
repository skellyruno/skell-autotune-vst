#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

struct Render { std::vector<float> in, out; int delay=0; };
Render generate(int blockSize)
{
    constexpr int sr=44100;
    constexpr int samples=sr*2;
    constexpr double pi=3.14159265358979323846;
    humtune::PitchShifter shifter;
    shifter.prepare(sr, blockSize);
    shifter.setMinFrequency(100);
    shifter.setLowLatencyExperiment(true);
    shifter.setLowLatencyBudgetMilliseconds(1.9);
    shifter.setFormantPreserve(false);
    Render v;
    v.delay=shifter.getLatencySamples();
    v.in.resize(samples);v.out.resize(samples);
    for(int i=0;i<samples;++i)
    {
        const double t=double(i)/sr;
        // Vocal-like harmonic partials, modulated in level rather than a single
        // constant sine that can hide grain-coverage errors.
        const double env=(.20+.14*std::sin(2*pi*1.3*t))
            * std::min(1.0,t/.02) * std::min(1.0,(2.0-t)/.02);
        v.in[i]=float(env*(std::sin(2*pi*210*t)
                 + .35*std::sin(2*pi*420*t+.3)
                 + .18*std::sin(2*pi*630*t+.8)));
    }
    for(int pos=0;pos<samples;)
    {
        const int n=std::min(blockSize,samples-pos);
        std::copy_n(v.in.data()+pos,n,v.out.data()+pos);
        shifter.process(v.out.data()+pos,n,float(sr/210.0),1.08f);
        pos+=n;
    }
    return v;
}
static double rms(const std::vector<float>& v,int first,int last)
{
    double s=0;
    for(int i=first;i<last;++i) s+=double(v[i])*v[i];
    return std::sqrt(s/(last-first));
}
int main()
{
    const auto a=generate(32);
    const auto b=generate(511);
    constexpr int sr=44100;
    double maxDiff=0.0;
    for(size_t i=0;i<a.out.size();++i)
        maxDiff=std::max(maxDiff,std::abs(double(a.out[i])-b.out[i]));
    double minDb=100.0,maxDb=-100.0;
    for(int first=int(.25*sr);first<int(1.75*sr);first+=int(.1*sr))
    {
        const double before=rms(a.in,first-a.delay,first+int(.1*sr)-a.delay);
        const double after=rms(a.out,first,first+int(.1*sr));
        const double db=20.0*std::log10(std::max(1.e-9,after)/std::max(1.e-9,before));
        minDb=std::min(minDb,db); maxDb=std::max(maxDb,db);
    }
    const bool ok=a.delay==b.delay && maxDiff<0.0002 && minDb>-2.8 && maxDb<2.0;
    std::printf("low latency weighted-coverage: delay=%d, host-size max sample diff %.7g, gain range %.2f..%.2f dB -> %s\n",
                a.delay,maxDiff,minDb,maxDb,ok?"PASS":"FAIL");
    return ok?0:1;
}
