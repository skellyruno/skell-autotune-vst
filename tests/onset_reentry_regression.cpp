#include <AutoTuneEngine.h>
#include <OnsetReentryGuard.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef NDEBUG
#error "Reentry behavior requires active assertions"
#endif

namespace {
using Engine = humtune::AutoTuneEngine;
struct Trace { std::vector<Engine::HopTrace> hops; size_t count = 0; };
struct Result { std::vector<float> audio; bool guarded = false, recovered = false; };
Result render(int block, bool formants)
{
    const auto fixture = std::filesystem::path(__FILE__).parent_path()
                       / "fixtures/moderate_harmonic_pop_6s.f32";
    std::ifstream file(fixture, std::ios::binary | std::ios::ate);
    assert(file && file.tellg() == std::streampos(6 * 44100 * 2 * sizeof(float)));
    Result result; result.audio.resize(6 * 44100 * 2);
    file.seekg(0); file.read(reinterpret_cast<char*>(result.audio.data()),
                            static_cast<std::streamsize>(result.audio.size() * sizeof(float)));
    assert(file);
    Engine engine; engine.setRangeProtection(true); engine.setVoiceRange(4);
    engine.setRootNote(7); engine.setScaleType(0); engine.setRetuneSpeed(0);
    engine.setAmount(1); engine.setHumanize(0); engine.setFlexTune(0);
    engine.setNoteStabilizer(false); engine.setFormantPreserve(formants);
    engine.setLowLatencyMode(true); engine.prepare(44100, block);
    assert(engine.getLatencySamples() == 84);
    Trace trace; trace.hops.resize(4096); // room for the optional 2 ms Low Latency hop
    engine.setOfflineTraceCallback([](void* context, const Engine::HopTrace& hop) noexcept {
        auto& t = *static_cast<Trace*>(context);
        assert(t.count < t.hops.size()); t.hops[t.count++] = hop;
    }, &trace);
    for (int offset = 0; offset < 6 * 44100; offset += block)
    {
        const int count = std::min(block, 6 * 44100 - offset);
        juce::AudioBuffer<float> buffer(2, count);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < count; ++i)
                buffer.getWritePointer(c)[i] = result.audio[2 * (offset + i) + c];
        engine.process(buffer);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < count; ++i) {
                const float x = buffer.getReadPointer(c)[i]; assert(std::isfinite(x));
                result.audio[2 * (offset + i) + c] = x;
            }
    }
    const auto at = [&](long long sample) -> const Engine::HopTrace& {
        const auto it = std::find_if(trace.hops.begin(), trace.hops.begin() + trace.count,
                                    [&](const auto& t) { return t.sample == sample; });
        assert(it != trace.hops.begin() + trace.count); return *it;
    };
    // In the untreated prefix, a weak post-consonant estimate briefly invents
    // D4 before returning to C3. Reject that first octave-confused acquisition,
    // but recover the real C3 two hops later and the later G3 within 8 ms.
    result.guarded = !at(127072).voiced && !at(144144).voiced && !at(144320).voiced;
    result.recovered = at(127424).voiced && at(127424).selectedTargetMidi == 48
                    && at(144496).voiced && at(144496).selectedTargetMidi == 55;
    if (formants) {
        assert(at(127072).voiced && at(127072).selectedTargetMidi == 62);
        assert(at(144144).voiced && at(144144).selectedTargetMidi == 55);
    }
    return result;
}
// A real post-consonant attack has a strong one-period fit (0.9234),
// while its changing pitch lowers the two-period fit to 0.4899.
// Rejecting it delays a valid G#3 and changes the subsequent synthesis phase.
Result renderStrongCycleReentry(int block)
{
    const auto fixture = std::filesystem::path(__FILE__).parent_path()
                       / "fixtures/strong_cycle_reentry_A_3s.f32";
    std::ifstream file(fixture, std::ios::binary | std::ios::ate);
    assert(file && file.tellg() == std::streampos(3 * 44100 * 2 * sizeof(float)));
    Result result; result.audio.resize(3 * 44100 * 2);
    file.seekg(0); file.read(reinterpret_cast<char*>(result.audio.data()),
                            static_cast<std::streamsize>(result.audio.size() * sizeof(float)));
    assert(file);
    Engine engine; engine.setRangeProtection(false); engine.setVoiceRange(2);
    engine.setRootNote(9); engine.setScaleType(0); engine.setRetuneSpeed(0);
    engine.setAmount(1); engine.setHumanize(0); engine.setFlexTune(0);
    engine.setNoteStabilizer(false); engine.setFormantPreserve(false);
    engine.setLowLatencyMode(true); engine.prepare(44100, block);
    assert(engine.getLatencySamples() == 84);
    Trace trace; trace.hops.resize(4096);
    engine.setOfflineTraceCallback([](void* context, const Engine::HopTrace& hop) noexcept {
        auto& state = *static_cast<Trace*>(context);
        assert(state.count < state.hops.size()); state.hops[state.count++] = hop;
    }, &trace);
    for (int offset = 0; offset < 3 * 44100; offset += block) {
        const int count = std::min(block, 3 * 44100 - offset);
        juce::AudioBuffer<float> buffer(2, count);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < count; ++i)
                buffer.getWritePointer(c)[i] = result.audio[2 * (offset + i) + c];
        engine.process(buffer);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < count; ++i) {
                const float x = buffer.getReadPointer(c)[i]; assert(std::isfinite(x));
                result.audio[2 * (offset + i) + c] = x;
            }
    }
    const auto event = std::find_if(trace.hops.begin(), trace.hops.begin() + trace.count,
        [](const auto& hop) { return hop.sample == 99088; });
    assert(event != trace.hops.begin() + trace.count);
    result.guarded = event->voiced && event->selectedTargetMidi == 56;
    std::printf("Strong-cycle reentry voiced=%d target=%g\n", event->voiced, event->selectedTargetMidi);
    return result;
}
}
int main()
{
    humtune::OnsetReentryGuard guard;
    guard.advance(true, false, .004); assert(!guard.hasTrustedOnset(.99f));
    guard.advance(true, true, .004); assert(!guard.hasTrustedOnset(.84f));
    assert(guard.hasTrustedOnset(.85f));
    for (int i = 0; i < 9; ++i) guard.advance(false, false, .004);
    assert(!guard.hasTrustedOnset(.99f));
    guard.advance(true, true, .004); guard.reset(); assert(!guard.hasTrustedOnset(.99f));
    assert(!guard.acceptsFirstPitch(.8f, .23f));
    assert(!guard.acceptsFirstPitch(.8f, -.05f)); // negative correlation is adverse evidence
    assert(guard.acceptsFirstPitch(.96f, .23f));
    assert(guard.acceptsFirstPitch(.8f, -1)); // unavailable evidence preserves tracking
    assert(guard.acceptsFirstPitch(.9f, .9f));
    auto reference = render(127, false);
    std::printf("Reentry rejected=%d recovered=%d\n", reference.guarded, reference.recovered);
    if (!reference.guarded || !reference.recovered) return 1;
    for (int block : {1, 512}) {
        const auto other = render(block, false);
        assert(other.guarded && other.recovered && other.audio == reference.audio);
    }
    render(127, true); // the deferred formant-preserving control path is retained
    const auto strong = renderStrongCycleReentry(127);
    if (!strong.guarded) return 2;
    for (int block : {1, 512}) {
        const auto other = renderStrongCycleReentry(block);
        assert(other.guarded && other.audio == strong.audio);
    }
}
