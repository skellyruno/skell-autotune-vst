// Regression for the verified G-major Fastest (180 Hz) dry-passthrough defect.
#include "AutoTuneEngine.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr double pi=3.14159265358979323846;
struct Result { float hz=0, confidence=0, ratio=1, effectiveFloor=0; int delay=0; float diff=0; };
static Result run(int block, int range, bool guard, bool lowLatency, double freq=160.0, int sampleRate=44100)
{
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(range);
    engine.setRangeProtection(guard);
    engine.setLowLatencyMode(lowLatency);
    engine.setRootNote(7); // G major
    engine.setScaleType(0);
    engine.setRetuneSpeed(0);
    engine.setAmount(1);
    engine.setHumanize(0);
    engine.setFlexTune(0);
    engine.setFormantPreserve(false);
    engine.prepare(sampleRate,block);
    // The real user example has harmonic-rich, somewhat irregular voiced input.
    // Two coherent harmonics keep this baseline deterministic across buffers.
    std::vector<float> out(static_cast<size_t>(sampleRate));
    int cursor=0;
    for (;cursor<sampleRate;)
    {
        const int n=std::min(block,sampleRate-cursor);
        juce::AudioBuffer<float> b(1,n);
        for(int i=0;i<n;++i)
        {
            const double t=double(cursor+i)/sampleRate;
            const float x=0.28f*std::sin(2*pi*freq*t)+0.10f*std::sin(4*pi*freq*t+.2);
            b.getWritePointer(0)[i]=x;
        }
        engine.process(b);
        for(int i=0;i<n;++i)out[static_cast<size_t>(cursor+i)]=b.getReadPointer(0)[i];
        cursor+=n;
    }
    Result r;
    r.hz=engine.getDetectedHz();r.confidence=engine.getConfidence();r.ratio=engine.getAppliedPitchRatio();
    r.delay=engine.getLatencySamples();r.effectiveFloor=engine.getEffectiveMinFrequency();
    const int begin=sampleRate/2;
    double error=0;
    for(int i=begin;i<sampleRate;++i)
    {
        const double t=double(i-r.delay)/sampleRate;
        const double dry=0.28*std::sin(2*pi*freq*t)+0.10*std::sin(4*pi*freq*t+.2);
        const double d=out[static_cast<size_t>(i)]-dry;
        error+=d*d;
    }
    r.diff=static_cast<float>(std::sqrt(error/(sampleRate-begin)));
    return r;
}
int main()
{
    for(const int sampleRate : {44100,48000,96000})
        for(const bool ll : {false,true})
        {
            const auto legacy=run(127,4,false,ll,160.0,sampleRate);
            const auto protected127=run(127,4,true,ll,160.0,sampleRate);
            const auto protected512=run(512,4,true,ll,160.0,sampleRate);
            const auto deep=run(127,1,false,ll,160.0,sampleRate);
            assert(legacy.effectiveFloor==180.0f);
            assert(protected127.effectiveFloor==80.0f);
            assert(protected512.effectiveFloor==80.0f);
            assert(protected127.delay==deep.delay);
            assert(protected127.delay==protected512.delay);
            if(!ll)assert(protected127.delay>legacy.delay);
            else assert(protected127.delay==legacy.delay);
            assert(std::abs(protected127.hz-160.0f)<3.0f);
            assert(protected127.confidence>0.7f);
            assert(std::abs(1200.0*std::log2(protected127.ratio))>10.0);
            assert(std::abs(protected127.hz-protected512.hz)<0.0001f);
            assert(std::abs(protected127.ratio-protected512.ratio)<0.00001f);
            assert(protected127.diff>0.006f); // Not raw passthrough while off-note.
            // Since the detector and the shifter use the same widened floor, the
            // protected Fastest audio must match the existing known-safe Deep mode.
            assert(std::abs(protected127.diff-deep.diff)<1.0e-6f);
            std::printf("%d Hz LL=%d legacy=%d safe=%d det=%.1f confidence=%.3f ratio=%.4f out-vs-dry %.4f PASS\n",
                 sampleRate,int(ll),legacy.delay,protected127.delay,protected127.hz,protected127.confidence,protected127.ratio,protected127.diff);
        }
}
