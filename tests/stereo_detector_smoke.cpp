#include "AutoTuneEngine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

int main() {
    constexpr double sr=48000.0, hz=220.0;
    humtune::AutoTuneEngine e;
    e.prepare(sr, 256);
    e.setVoiceRange(2); // 100 Hz floor
    e.setEnabled(true); // exercise detector path
    juce::AudioBuffer<float> b(2, 48000);
    for(int i=0;i<b.getNumSamples();++i) {
        float x=0.4f*std::sin(2.0*3.141592653589793*hz*i/sr);
        b.getWritePointer(0)[i]=x;
        b.getWritePointer(1)[i]=-x;
    }
    // Feed in small host blocks, like a real-time callback.
    for(int p=0;p<b.getNumSamples();p+=256) {
        juce::AudioBuffer<float> chunk(2,std::min(256,b.getNumSamples()-p));
        for(int c=0;c<2;++c) std::copy(b.getReadPointer(c)+p,b.getReadPointer(c)+p+chunk.getNumSamples(),chunk.getWritePointer(c));
        e.process(chunk);
    }
    std::printf("stereo anti-phase detector result %.2f Hz confidence %.3f\n",e.getDetectedHz(),e.getConfidence());
    return (std::abs(e.getDetectedHz()-hz)<4.0f) ? 0 : 1;
}
