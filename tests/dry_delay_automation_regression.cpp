#include "AutoTuneEngine.h"
#ifdef NDEBUG
#error "DSP regression assertions must stay enabled in Release"
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

// Every dry sample is indexed against absolute input time, including latency
// changes and bypass toggles. The changing latency invalidates only the old
// history horizon, not the allocation. Different callback partitions must
// produce bit-identical results.
static std::vector<float> render(double sr, int block)
{
    humtune::AutoTuneEngine e;
    e.setVoiceRange(5);
    e.setLowLatencyMode(false);
    e.setEnabled(false);
    e.prepare(sr, block);
    constexpr int channels=2;
    const int n=static_cast<int>(sr*0.8);
    std::vector<float> output(static_cast<size_t>(n*channels));
    int lastReset=0;
    int currentDelay=e.getLatencySamples();
    for(int pos=0;pos<n;)
    {
        // All control events land on fixed absolute sample indices, never a
        // buffer boundary chosen by the host.
        if(pos==static_cast<int>(sr*.20)) {
            e.setLowLatencyMode(true); lastReset=pos; currentDelay=e.getLatencySamples();
        }
        if(pos==static_cast<int>(sr*.42)) {
            e.setVoiceRange(3); lastReset=pos; currentDelay=e.getLatencySamples();
        }
        if(pos==static_cast<int>(sr*.61)) {
            e.setLowLatencyMode(false); lastReset=pos; currentDelay=e.getLatencySamples();
        }
        int count=std::min(block,n-pos);
        for(double edge:{sr*.20,sr*.42,sr*.61})
            if(pos<static_cast<int>(edge)) count=std::min(count,static_cast<int>(edge)-pos);
        juce::AudioBuffer<float> b(channels,count);
        for(int ch=0;ch<channels;++ch)
            for(int i=0;i<count;++i)
                b.getWritePointer(ch)[i]=static_cast<float>((ch? -0.3:0.4)*
                    std::sin(0.00071*(pos+i)+0.19*ch) + 0.00001*((pos+i)%37));
        e.process(b);
        for(int i=0;i<count;++i) for(int ch=0;ch<channels;++ch)
        {
            const int t=pos+i;
            const int source=t-currentDelay;
            const float expected=source>=lastReset
                ? static_cast<float>((ch? -0.3:0.4)*
                    std::sin(0.00071*source+0.19*ch) + 0.00001*(source%37)) : 0.0f;
            const float result=b.getReadPointer(ch)[i];
            assert(std::abs(result-expected)<1.e-6f);
            output[static_cast<size_t>(t*channels+ch)]=result;
        }
        pos+=count;
    }
    return output;
}

// Toggling the effect does not stop the underlying dry history. The first
// disabled sample must correspond to input[t - latency], even if that sample
// originally passed through the enabled pitch shifter.
static void verifyEnabledThenBypass(int block)
{
    constexpr double sr=48000.0;
    humtune::AutoTuneEngine e;
    e.setVoiceRange(2);
    e.setLowLatencyMode(false);
    e.setEnabled(true);
    e.prepare(sr,block);
    const int latency=e.getLatencySamples();
    const int total=static_cast<int>(sr*.75);
    const int off1=static_cast<int>(sr*.20);
    const int onAgain=static_cast<int>(sr*.37);
    const int off2=static_cast<int>(sr*.55);
    for(int pos=0;pos<total;)
    {
        if(pos==off1 || pos==off2) e.setEnabled(false);
        if(pos==onAgain) e.setEnabled(true);
        int n=std::min(block,total-pos);
        for(int edge:{off1,onAgain,off2})
            if(pos<edge) n=std::min(n,edge-pos);
        juce::AudioBuffer<float> audio(2,n);
        for(int ch=0;ch<2;++ch)
            for(int i=0;i<n;++i)
                audio.getWritePointer(ch)[i]=static_cast<float>(
                    (ch==0?.32:-.24)*std::sin((pos+i)*.0083+.33*ch));
        e.process(audio);
        if((pos>=off1&&pos<onAgain)||pos>=off2)
            for(int ch=0;ch<2;++ch)
                for(int i=0;i<n;++i)
                {
                    const int src=pos+i-latency;
                    const float wanted=src<0?0.0f:static_cast<float>(
                        (ch==0?.32:-.24)*std::sin(src*.0083+.33*ch));
                    assert(std::abs(audio.getReadPointer(ch)[i]-wanted)<1.0e-5f);
                }
        pos+=n;
    }
}
int main()
{
    verifyEnabledThenBypass(127);
    verifyEnabledThenBypass(512);
    for(double sr:{44100.,48000.,96000.})
    {
        const auto a=render(sr,1);
        const auto b=render(sr,127);
        const auto c=render(sr,1024);
        assert(a.size()==b.size() && a.size()==c.size());
        for(size_t i=0;i<a.size();++i) {
            assert(a[i]==b[i]); assert(a[i]==c[i]);
        }
        std::printf("%.0f Hz: sample-accurate dry bypass across latency/range automation, 1/127/1024 blocks PASS\n",sr);
    }
}
