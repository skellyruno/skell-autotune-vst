#include "AutoTuneEngine.h"
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
std::atomic<bool> gTrackAllocations { false };
std::atomic<unsigned long long> gAllocationCount { 0 };

void noteAllocation() noexcept
{
    if (gTrackAllocations.load(std::memory_order_relaxed))
        gAllocationCount.fetch_add(1, std::memory_order_relaxed);
}
}

void* operator new (std::size_t size)
{
    noteAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size)
{
    noteAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free(p); }
void operator delete[] (void* p) noexcept { std::free(p); }
void operator delete (void* p, std::size_t) noexcept { std::free(p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free(p); }
void* operator new (std::size_t size, std::align_val_t alignment)
{
    noteAllocation();
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* p = _aligned_malloc(size == 0 ? align : size, align)) return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, align, size == 0 ? align : size) == 0) return p;
#endif
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete (void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[] (void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete (void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[] (void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

int main()
{
    constexpr double sr = 48000.0;
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(5);
    engine.setLowLatencyMode(true);
    engine.setEnabled(true);
    engine.prepare(sr, 1024);

    // Allocate all host-buffer stand-ins before counting. This test intentionally tracks
    // the engine and mode/range changes, not host buffer construction by the test itself.
    juce::AudioBuffer<float> b64(2, 64), b127(2, 127), b256(2, 256), b1024(2, 1024);
    std::array<juce::AudioBuffer<float>*, 4> blocks { &b64, &b127, &b256, &b1024 };
    for (int warm = 0; warm < 12; ++warm)
    {
        auto& b = *blocks[static_cast<std::size_t>(warm % 4)];
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.getWritePointer(ch)[i] = 0.2f * std::sin(
                    2.0 * juce::MathConstants<double>::pi * 220.0 * (i + warm * 1024) / sr);
        engine.process(b);
    }

    gAllocationCount.store(0, std::memory_order_relaxed);
    gTrackAllocations.store(true, std::memory_order_relaxed);
    for (int iter = 0; iter < 80; ++iter)
    {
        // Exercise normal callbacks with heterogeneous buffers.
        auto& b = *blocks[static_cast<std::size_t>(iter % 4)];
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.getWritePointer(ch)[i] = 0.2f * std::sin(
                    2.0 * juce::MathConstants<double>::pi * 220.0 * (i + iter * 1024) / sr);
        engine.process(b);

        // The production controls can be automated while processing. Verify their
        // current preallocated reset paths as part of the same no-heap-allocation gate.
        if (iter % 10 == 4)
            engine.setLowLatencyMode(!engine.getLowLatencyMode());
        if (iter % 8 == 3)
            engine.setVoiceRange((iter / 8) % humtune::AutoTuneEngine::kNumRanges);
        if (iter % 7 == 1)
            engine.setRangeProtection(!engine.getRangeProtection());
        if (iter % 6 == 2)
            engine.setFormantShiftSemitones(static_cast<float>((iter % 9) - 4));
    }
    // Keep counting while a weak fundamental forces fractional-period evidence.
    // The host buffer is reused; neither this signal change nor the detector's
    // additional waveform comparisons may allocate on the processing thread.
    for (int iter = 0; iter < 120; ++iter)
    {
        for (int ch = 0; ch < b127.getNumChannels(); ++ch)
            for (int i = 0; i < b127.getNumSamples(); ++i)
            {
                const double phase = 2.0 * juce::MathConstants<double>::pi * 240.0
                                   * (iter * 127 + i) / sr;
                b127.getWritePointer(ch)[i] = static_cast<float>(
                    .02 * std::sin(phase) + .02 * std::sin(2 * phase + .3)
                    + .2 * std::sin(3 * phase + .5));
            }
        engine.process(b127);
    }
    gTrackAllocations.store(false, std::memory_order_relaxed);

    const auto allocations = gAllocationCount.load(std::memory_order_relaxed);
    const bool pass = allocations == 0;
    std::printf("realtime allocation audit: 80 mixed callbacks plus 120 weak-root callbacks, 10 range changes, 8 latency-mode changes, range safety toggling, formant-shift automation -> %llu C++ heap allocations %s\n",
                allocations, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
