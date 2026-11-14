#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Real-time-safe phase anchoring regression. Unlike the old synthetic F0-only
// probe this input contains multiple harmonics and an intentional note change.
// The output must be deterministic across different DAW callback sizes and
// must keep its predictive grain coverage when phase locking is enabled.
struct Result {
    std::vector<float> samples;
    float meanCoverage = 0.0f;
    float maxStep = 0.0f;
    int latency = 0;
};
static Result render(double sr, double hz, int block)
{
    humtune::PitchShifter s;
    s.prepare(sr, block);
    s.setMinFrequency(70.0);
    s.setLowLatencyExperiment(true);
    s.setLowLatencyBudgetMilliseconds(1.9);
    s.setPhaseLockedSource(true);
    s.setFormantPreserve(false);
    const int count = static_cast<int>(sr * 0.55);
    Result r;
    r.samples.resize(static_cast<size_t>(count));
    r.latency = s.getLatencySamples();
    constexpr double pi = 3.14159265358979323846;
    double phase = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double inputHz = i < count/2 ? hz : hz*1.1224620483;
        phase += 2*pi*inputHz/sr;
        r.samples[static_cast<size_t>(i)] = static_cast<float>(0.19 * (
            std::sin(phase) + .35*std::sin(2*phase + .22)
            + .18*std::sin(3*phase + .55)));
    }
    double sumCoverage=0.0;int observations=0;
    for(int i=0;i<count;)
    {
        // The parameter step is exactly at count/2 for *all* host block sizes;
        // a test that automates only at callback boundaries is not a fair test.
        const int remainingBeforeStep=i<count/2?count/2-i:count-i;
        const int n=std::min({block,count-i,remainingBeforeStep});
        const bool changed=i>=count/2;
        const float f0=static_cast<float>(changed?hz*1.1224620483:hz);
        s.process(r.samples.data()+i,n,static_cast<float>(sr/f0),changed?1.08f:.94f);
        const auto diag=s.getGrainDiagnostics();
        if(i>static_cast<int>(sr*.12) && i<static_cast<int>(sr*.5))
        {
            sumCoverage+=diag.coverage;
            ++observations;
        }
        i+=n;
    }
    r.meanCoverage=static_cast<float>(sumCoverage/std::max(1,observations));
    for(int i=1;i<count;++i)
        r.maxStep=std::max(r.maxStep,std::abs(r.samples[static_cast<size_t>(i)]-r.samples[static_cast<size_t>(i-1)]));
    return r;
}
int main()
{
    bool okay=true;
    for(double sr : {44100.0,48000.0,96000.0})
        for(double hz : {110.0,220.0,390.0})
        {
            const auto a=render(sr,hz,32);
            const auto b=render(sr,hz,511);
            double diff=0.0;
            for(size_t i=0;i<a.samples.size();++i)
            {
                if(!std::isfinite(a.samples[i])||!std::isfinite(b.samples[i]))
                    return 2;
                diff=std::max(diff,std::abs(static_cast<double>(a.samples[i]-b.samples[i])));
            }
            const bool pass=diff<2.0e-4 && a.latency==b.latency
                && a.meanCoverage>.88f && b.meanCoverage>.88f
                && a.maxStep<.40f && b.maxStep<.40f;
            std::printf("phase-locked %.0fHz @ %.0fHz: max diff %.6g, coverage %.3f/%.3f step %.3f/%.3f %s\n",
                hz,sr,diff,a.meanCoverage,b.meanCoverage,a.maxStep,b.maxStep,pass?"PASS":"FAIL");
            okay=pass && okay;
        }
    return okay?0:1;
}
