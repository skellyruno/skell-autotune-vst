#include "PitchShifter.h"
#ifdef NDEBUG
#error "Enable assertions in regression builds"
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static std::vector<float> render(double sr, bool enabled)
{
    humtune::PitchShifter p;
    p.prepare(sr,127);
    p.setMinFrequency(100);
    p.setLowLatencyExperiment(true);
    p.setLowLatencyBudgetMilliseconds(1.9);
    p.setPhaseLockedSource(true);
    p.setDetailedDiagnosticsEnabled(enabled);
    p.setFormantPreserve(false);
    const int n=static_cast<int>(sr*.6);
    std::vector<float> a(n);
    double phase=0.0;
    constexpr double tau=6.2831853071795864769;
    for(int i=0;i<n;++i)
    {
        const double f = i < n/2 ? 185.0 : 267.0;
        phase+=tau*f/sr;
        a[i]=static_cast<float>(0.2*std::sin(phase)+0.1*std::sin(2*phase+0.19));
    }
    for(int pos=0;pos<n;)
    {
        const int count=std::min({127,n-pos,pos<n/2?n/2-pos:127});
        const float freq=pos<n/2 ? 185.0f : 267.0f;
        p.process(a.data()+pos,count,static_cast<float>(sr/freq),1.0594631f);
        const auto d=p.getGrainDiagnostics();
        assert(std::isfinite(d.cancellationFraction));
        assert(d.cancellationFraction>=0.f && d.cancellationFraction<=1.f);
        pos+=count;
    }
    return a;
}
int main()
{
    for(double sr:{44100.,96000.})
    {
        const auto a=render(sr,false),b=render(sr,true);
        assert(a.size()==b.size());
        for(size_t i=0;i<a.size();++i) assert(a[i]==b[i]);
        std::printf("%.0f Hz: offline cancellation tracing leaves audio bit-identical\n",sr);
    }
}
