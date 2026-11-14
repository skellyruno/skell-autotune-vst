#include "AutoTuneEngine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Offline timing tool, deliberately separate from CTest: desktop scheduling
// cannot establish a portable hard real-time guarantee. All buffers and timing
// storage are prepared outside the measured callback. Color automation occurs
// at the same 25 ms sample boundaries for every host block size.
int main(int argc, char** argv)
{
    if (argc != 7 && argc != 8) {
        std::fprintf(stderr, "usage: stress sampleRate block lowLatency formants range seconds [pitchMotion]\n");
        return 2;
    }
    const int sr = std::atoi(argv[1]), block = std::atoi(argv[2]);
    const bool low = std::atoi(argv[3]) != 0, formants = std::atoi(argv[4]) != 0;
    const int range = std::atoi(argv[5]);
    const double seconds = std::atof(argv[6]);
    const bool motion = argc == 8 && std::atoi(argv[7]) != 0;
    if (sr < 8000 || sr > 192000 || block <= 0 || block > 8192 ||
        range < 0 || range >= humtune::AutoTuneEngine::kNumRanges || seconds < 1 || seconds > 60)
        return 2;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(range);
    engine.setLowLatencyMode(low);
    engine.setScaleType(2); // Explicit custom scale below, independent of enum names.
    std::array<bool,12> chromatic; chromatic.fill(true);
    engine.setCustomScale(chromatic);
    engine.setRetuneSpeed(0); engine.setAmount(1); engine.setHumanize(0);
    engine.setNoteStabilizer(false); engine.setFormantPreserve(formants);
    if (motion) { engine.setAmount(.5f); engine.setGlideMilliseconds(25); }
    engine.prepare(sr, block);
    const int automationHop = sr / 40;
    const int frames = static_cast<int>(sr * (seconds + 0.5));
    const double source = humtune::AutoTuneEngine::kRangeMinHz[range] * (motion ? 1.5 : 1.027);
    std::vector<float> input(static_cast<size_t>(frames) * 2);
    constexpr double tau = 6.2831853071795864769;
    double movingPhase = 0;
    for (int i = 0; i < frames; ++i) {
        movingPhase += tau*source*std::exp2(3*std::sin(tau*3*i/sr)/12)/sr;
        const double p = motion ? movingPhase : tau * source * i / sr;
        input[2 * static_cast<size_t>(i)] = static_cast<float>(.16*std::sin(p)+.06*std::sin(3*p+.2)+.03*std::sin(11*p+.5));
        input[2 * static_cast<size_t>(i)+1] = static_cast<float>(.15*std::sin(p+.18)+.05*std::sin(4*p+.4)+.025*std::sin(13*p+.7));
    }
    // The stub AudioBuffer owns memory; one preallocated instance for each
    // possible short segment avoids measuring host buffer construction.
    std::vector<juce::AudioBuffer<float>> buffers;
    buffers.reserve(static_cast<size_t>(block));
    for (int n = 1; n <= block; ++n) buffers.emplace_back(2,n);
    std::vector<double> times, loads;
    times.reserve(static_cast<size_t>(frames/block + frames/automationHop + 8));
    loads.reserve(times.capacity());
    double total = 0, peak = 0; int late = 0, measured = 0;
    for (int offset = 0; offset < frames;) {
        const int n = std::min({block, frames-offset, automationHop-offset%automationHop});
        auto& buffer = buffers[static_cast<size_t>(n-1)];
        for (int ch=0;ch<2;++ch) for(int i=0;i<n;++i)
            buffer.getWritePointer(ch)[i] = input[2*static_cast<size_t>(offset+i)+ch];
        const auto start = std::chrono::steady_clock::now();
        if (offset % automationHop == 0)
            engine.setFormantShiftSemitones(((offset/automationHop)&1) ? 4.0f : -4.0f);
        engine.process(buffer);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        if (offset >= sr/2) {
            times.push_back(elapsed); loads.push_back(elapsed*sr/n);
            total += elapsed; measured += n; late += elapsed > double(n)/sr;
        }
        for (int ch=0;ch<2;++ch) for (int i=0;i<n;++i) {
            const float x=buffer.getReadPointer(ch)[i];
            if (!std::isfinite(x)) return 3;
            peak=std::max(peak,double(std::abs(x)));
        }
        offset += n;
    }
    std::sort(times.begin(),times.end()); std::sort(loads.begin(),loads.end());
    const auto q = [](const std::vector<double>& v,double p) {return v[static_cast<size_t>(p*(v.size()-1))];};
    std::printf("rate,block,low,formants,range,source_hz,latency,duty,p99_seconds,max_seconds,p99_deadline_fraction,max_deadline_fraction,late_callbacks,callbacks,peak,pitch_motion\n");
    std::printf("%d,%d,%d,%d,%d,%.8g,%d,%.8g,%.8g,%.8g,%.8g,%.8g,%d,%zu,%.8g,%d\n",
        sr,block,low,formants,range,source,engine.getLatencySamples(),total*sr/measured,
        q(times,.99),times.back(),q(loads,.99),loads.back(),late,times.size(),peak,motion);
}
