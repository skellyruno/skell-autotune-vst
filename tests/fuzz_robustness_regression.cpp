// Robustness: hostile inputs, odd block sizes, randomised settings and mode toggles must never produce
// non-finite or runaway output (and, under ASan/UBSan, no memory or undefined-behaviour errors).
// Deterministic (fixed seeds). Covers the Low Latency fast response, the Standard continuity rescue,
// the full-window retry and the optional 2 ms hop.
#include "AutoTuneEngine.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
struct Rng
{
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 2685821657736338717ULL + 88172645463325252ULL) {}
    std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double u() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1)); }
};
constexpr double kPi = 3.14159265358979323846;

// kind 0..11, returns one mono sample stream
std::vector<float> makeSignal(int kind, double sr, double seconds, Rng& rng)
{
    const int n = static_cast<int>(seconds * sr);
    std::vector<float> x(static_cast<size_t>(n), 0.0f);
    double ph = 0.0, ph2 = 0.0;
    double f = 150.0 + 200.0 * rng.u();
    int segLeft = 0; bool on = true;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        double v = 0.0;
        switch (kind)
        {
            case 0: v = (rng.u() * 2.0 - 1.0) * std::pow(10.0, -3.0 * rng.u() * 0.0 - 0.8); break;                    // loud white noise
            case 1: v = (rng.range(0, 150) == 0) ? 0.9 * (rng.u() * 2.0 - 1.0) : 0.0; break;                             // sparse impulses
            case 2: ph += 2 * kPi * (110.0 + 770.0 * std::fmod(t * 2.0, 1.0)) / sr; v = std::sin(ph) >= 0 ? 0.9 : -0.9; break; // square, sweeping
            case 3: { const double fx = (i < n / 2) ? 80.0 + 1900.0 * (t / (seconds * 0.5)) : 1980.0 - 1900.0 * ((t - seconds * 0.5) / (seconds * 0.5)); ph += 2 * kPi * fx / sr; v = 0.6 * std::sin(ph); break; } // chirps
            case 4: { if (--segLeft <= 0) { segLeft = static_cast<int>(sr * (0.01 + 0.03 * rng.u())); f *= std::pow(2.0, (rng.u() * 24.0 - 12.0) / 12.0); f = std::fmin(900.0, std::fmax(70.0, f)); }
                      ph += 2 * kPi * f / sr; v = 0.4 * std::sin(ph) + 0.2 * std::sin(2 * ph) + 0.1 * std::sin(3 * ph); break; } // pitch jumps incl. octaves
            case 5: { if (--segLeft <= 0) { segLeft = static_cast<int>(sr * (0.001 + 0.05 * rng.u())); on = !on; if (on) f = 90.0 + 400.0 * rng.u(); }
                      ph += 2 * kPi * f / sr; v = on ? 0.5 * std::sin(ph) + 0.2 * std::sin(2 * ph) : 0.0; break; }        // tone/silence bursts
            case 6: ph += 2 * kPi * 200.0 / sr; v = std::fmax(-1.0, std::fmin(1.0, 6.0 * std::sin(ph) + 0.3)); break;     // clipped + DC
            case 7: ph += 2 * kPi * 220.0 / sr; v = 1.0e-30 * std::sin(ph) + 1.0e-38 * (rng.u() - 0.5); break;            // denormal level
            case 8: { const bool lowSeg = std::fmod(t, 0.1) < 0.05; ph += 2 * kPi * (lowSeg ? 30.0 : 4000.0) / sr; v = 0.5 * std::sin(ph); break; } // extreme pitches
            case 9: { const int step = static_cast<int>(t * 50.0); v = (step % 2 ? 0.8 : -0.8) * ((i & 1) ? 1.0 : (step % 3 ? 1.0 : -1.0)); break; } // DC steps / Nyquist-ish
            case 10: { ph += 2 * kPi * 180.0 / sr; v = 0.006 * (std::sin(ph) + 0.5 * std::sin(2 * ph)) + 0.004 * (rng.u() * 2.0 - 1.0); break; } // weak breathy
            default: ph += 2 * kPi * 196.0 / sr; ph2 += 2 * kPi * 311.0 / sr; v = (0.3 + 0.3 * std::sin(2 * kPi * 7.0 * t)) * (std::sin(ph) + 0.8 * std::sin(ph2)); break; // chord, AM
        }
        x[static_cast<size_t>(i)] = static_cast<float>(v);
    }
    return x;
}
} // namespace

int main()
{
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    int cases = 0; long long samples = 0;
    for (int kind = 0; kind < 12; ++kind)
        for (int ri = 0; ri < 3; ++ri)
            for (int pattern = 0; pattern < 4; ++pattern)
            {
                const double sr = rates[ri];
                Rng rng(static_cast<std::uint64_t>(1 + kind * 1000 + ri * 100 + pattern * 10));
                const auto sig = makeSignal(kind, sr, 0.45, rng);

                humtune::AutoTuneEngine engine;
                engine.setVoiceRange(rng.range(0, 4));
                engine.setRootNote(rng.range(0, 11));
                engine.setScaleType(rng.range(0, 3));
                engine.setAmount(static_cast<float>(0.5 + 0.5 * rng.u()));
                engine.setRetuneSpeed(static_cast<float>(rng.u()));
                engine.setHumanize(static_cast<float>(rng.u()));
                engine.setFormantPreserve(rng.range(0, 1) != 0);
                engine.setNoteStabilizer(rng.range(0, 3) == 0);
                engine.setLowLatencyMode(rng.range(0, 1) != 0);
                engine.setLowLatencyFastHop(rng.range(0, 1) != 0);
                engine.setEnabled(true);
                const int maxBlock = 2048;
                engine.prepare(sr, maxBlock);

                size_t pos = 0; int calls = 0;
                while (pos < sig.size())
                {
                    int block = pattern == 0 ? 1 : pattern == 1 ? 7 : pattern == 2 ? 127 : rng.range(1, maxBlock);
                    block = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), sig.size() - pos));
                    juce::AudioBuffer<float> buf(2, block);
                    for (int i = 0; i < block; ++i)
                    {
                        const float v = sig[pos + static_cast<size_t>(i)];
                        buf.getWritePointer(0)[i] = v;
                        buf.getWritePointer(1)[i] = v;
                    }
                    // random mid-stream changes, as host automation could do
                    if (calls % 41 == 17) engine.setLowLatencyMode(!engine.getLowLatencyMode());
                    if (calls % 29 == 11) engine.setLowLatencyFastHop(!engine.getLowLatencyFastHop());
                    if (calls % 53 == 5)  engine.setVoiceRange(rng.range(0, 4));
                    engine.process(buf);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                        {
                            const float y = buf.getReadPointer(ch)[i];
                            if (!(std::isfinite(y) && std::abs(y) < 8.0f))
                            {
                                std::printf("FAIL kind=%d sr=%.0f pattern=%d call=%d sample=%d value=%g\n", kind, sr, pattern, calls, i, static_cast<double>(y));
                                assert(false);
                            }
                        }
                    pos += static_cast<size_t>(block); samples += block; ++calls;
                }
                ++cases;
            }
    std::printf("fuzz_robustness_regression: %d cases, %lld samples per channel, all output finite and bounded PASS\n", cases, samples);
    return 0;
}
