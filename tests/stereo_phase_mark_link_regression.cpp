#include "AutoTuneEngine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef NDEBUG
#error "Release quality assertions must remain enabled"
#endif

namespace {
constexpr int kSampleRate = 44100;
constexpr int kSamples = 4 * kSampleRate;
constexpr double kPi = 3.14159265358979323846;
using Signal = std::vector<float>;

Signal loadFixture()
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "near_dual_mono_4s.f32";
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    assert(stream.good());
    assert(stream.tellg() == std::streampos(kSamples * 2 * sizeof(float)));
    stream.seekg(0);
    Signal input(static_cast<size_t>(kSamples) * 2);
    stream.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(input.size() * sizeof(float)));
    assert(stream.good());
    return input;
}

Signal process(const Signal& input, int hostBlock, bool lowLatency, bool automate = false)
{
    humtune::AutoTuneEngine e;
    e.setRangeProtection(true);
    e.setVoiceRange(4); // Fastest range protected down to 80 Hz
    e.setRootNote(7);  // G major, maximum retune
    e.setScaleType(0);
    e.setRetuneSpeed(0.0f);
    e.setAmount(1.0f);
    e.setHumanize(0.0f);
    e.setNoteStabilizer(false);
    e.setFormantPreserve(false);
    e.setLowLatencyMode(lowLatency);
    e.setEnabled(true);
    e.prepare(kSampleRate, hostBlock);
    assert(e.getLatencySamples() == (lowLatency ? 84 : 726));

    Signal output(input.size());
    for (int start = 0; start < kSamples; )
    {
        if (automate && start == kSampleRate)
            e.setLowLatencyMode(!lowLatency);
        if (automate && start == 2 * kSampleRate)
        {
            e.setLowLatencyMode(lowLatency);
            e.setVoiceRange(3);
        }
        if (automate && start == 3 * kSampleRate)
            e.setVoiceRange(4);
        const int toBoundary = automate
            ? kSampleRate - (start % kSampleRate) : hostBlock;
        const int count = std::min({hostBlock, kSamples - start, toBoundary});
        juce::AudioBuffer<float> buffer(2, count);
        for (int i = 0; i < count; ++i)
            for (int ch = 0; ch < 2; ++ch)
                buffer.getWritePointer(ch)[i] = input[static_cast<size_t>(start + i) * 2 + ch];
        e.process(buffer);
        for (int i = 0; i < count; ++i)
            for (int ch = 0; ch < 2; ++ch)
                output[static_cast<size_t>(start + i) * 2 + ch] = buffer.getReadPointer(ch)[i];
        start += count;
    }
    return output;
}

struct Stats { double rmsLeft, rmsRight, difference, correlation, foldRms; };
Stats stats(const Signal& signal)
{
    double ll=0, rr=0, lr=0, dd=0, mm=0;
    for (int i = 0; i < kSamples; ++i)
    {
        const double l = signal[static_cast<size_t>(i) * 2];
        const double r = signal[static_cast<size_t>(i) * 2 + 1];
        assert(std::isfinite(l) && std::isfinite(r));
        ll += l * l; rr += r * r; lr += l * r;
        dd += (l-r)*(l-r);
        mm += .25*(l+r)*(l+r);
    }
    assert(ll > 1.0e-6 && rr > 1.0e-6);
    const double n = static_cast<double>(kSamples);
    return {std::sqrt(ll/n), std::sqrt(rr/n), std::sqrt(dd/n),
            lr/std::sqrt(ll*rr), std::sqrt(mm/n)};
}

void checkConsistent(const Signal& a, const Signal& b)
{
    assert(a.size() == b.size());
    for (size_t i=0;i<a.size();++i)
        assert(std::abs(a[i]-b[i]) < 1.0e-6f);
}
}

int main()
{
    const auto input=loadFixture();
    const auto original=stats(input);
    assert(original.correlation > 0.999999);
    for (bool low : {false, true})
    {
        const auto first = process(input,127,low);
        const auto second = process(input,512,low);
        checkConsistent(first,second);
        const auto result=stats(first);
        // A spurious, independently chosen right-channel grain mark caused
        // up to ~60% correlation loss on actual dual-mono vocals before v39.
        assert(result.correlation > 0.999999);
        assert(result.difference < 1.0e-5);
        assert(result.foldRms > 0.995 * result.rmsLeft);
        // Switching latency and voice-range resets both shifters while the
        // stereo mark history remains allocated. No stale ring entry may be
        // consumed by the follower during or after these state resets.
        const auto automatedA = process(input,127,low,true);
        const auto automatedB = process(input,512,low,true);
        checkConsistent(automatedA,automatedB);
        const auto automationStats = stats(automatedA);
        assert(automationStats.correlation > 0.999999);
        assert(automationStats.difference < 1.0e-5);
        std::cout << "mode=" << low << " corr=" << result.correlation
                  << " fold=" << result.foldRms << " rmsDifference=" << result.difference << '\n';
    }
    // A common pitch-mark timeline must NOT copy one output channel onto
    // the other. Each channel retains its independent waveform and amplitude.
    auto genuineStereo=input;
    for (int i=0;i<kSamples;++i)
    {
        const auto k=static_cast<size_t>(i)*2;
        const double time=double(i)/kSampleRate;
        genuineStereo[k+1] = 0.60f*genuineStereo[k]
            + static_cast<float>(0.055*std::sin(2.0*kPi*330.0*time));
    }
    const auto distinct=stats(process(genuineStereo,127,true));
    assert(distinct.difference > 0.01);
    assert(distinct.correlation < 0.999);
    std::cout << "true stereo difference=" << distinct.difference << '\n';
}
