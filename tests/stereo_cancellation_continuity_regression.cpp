#include "AutoTuneEngine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static std::vector<float> render(int block, bool low)
{
    constexpr int sampleRate = 44100;
    constexpr double pi = 3.14159265358979323846;
    humtune::AutoTuneEngine e;
    e.setRootNote(7);
    e.setScaleType(0);
    e.setRetuneSpeed(0);
    e.setVoiceRange(4);
    e.setRangeProtection(true);
    e.setLowLatencyMode(low);
    e.setFormantPreserve(false);
    e.prepare(sampleRate, block);
    const int count = sampleRate;
    std::vector<float> out(static_cast<size_t>(count));
    for (int pos=0; pos<count;)
    {
        const int n = std::min(block,count-pos);
        juce::AudioBuffer<float> b(2,n);
        for (int i=0; i<n; ++i)
        {
            const double t = double(pos+i)/sampleRate;
            const float voice = float(.23*std::sin(2*pi*165*t)+.11*std::sin(4*pi*165*t+.4));
            // Sweep smoothly across a cancellation threshold; phase and source
            // selection must not depend on host callback boundaries.
            const float anti = float(.8 + .15*std::sin(2*pi*.8*t));
            b.getWritePointer(0)[i] = voice;
            b.getWritePointer(1)[i] = -anti*voice + .025f*std::sin(float(2*pi*211*t));
        }
        e.process(b);
        for(int i=0;i<n;++i)
        {
            const float x=b.getReadPointer(0)[i];
            assert(std::isfinite(x));
            out[static_cast<size_t>(pos+i)]=x;
        }
        pos+=n;
    }
    assert(e.getConfidence()>0.5f);
    return out;
}
int main()
{
    for (bool low:{false,true})
    {
        auto a=render(1,low), b=render(127,low), c=render(512,low);
        double maxDiff=0;
        for(size_t i=0;i<a.size();++i)
            maxDiff=std::max({maxDiff, std::abs(double(a[i]-b[i])),std::abs(double(a[i]-c[i]))});
        std::printf("stereo cancellation mode=%d max diff %.9g\n",int(low),maxDiff);
        assert(maxDiff < 1.e-6);
    }
}
