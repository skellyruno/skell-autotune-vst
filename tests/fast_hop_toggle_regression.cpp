// The optional Low Latency fast hop (2 ms instead of 4 ms) can be toggled from the host while audio
// runs. The engine accumulates audio per hop, so a hop that shrinks under a partly filled accumulator
// must never produce a negative segment (a read before the host buffer). Exercise that at several
// sample rates and block sizes and require finite, bounded output. Meaningful under ASan/UBSan too.
#include "AutoTuneEngine.h"
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    const int blocks[] = { 32, 64, 127, 256, 1024 };
    long long processed = 0;
    for (const double sr : rates)
        for (const int block : blocks)
        {
            humtune::AutoTuneEngine engine;
            engine.setVoiceRange(4);
            engine.setLowLatencyMode(true);
            engine.setLowLatencyFastHop(false);
            assert(!engine.getLowLatencyFastHop());
            engine.setEnabled(true);
            engine.prepare(sr, block);

            juce::AudioBuffer<float> buf(2, block);
            double phase = 0.0;
            const int totalBlocks = static_cast<int>(2.0 * sr / block);
            for (int b = 0; b < totalBlocks; ++b)
            {
                const double t = b * block / sr;
                // Voiced notes with vibrato and a jump at 1 s, silent gap 0.55-0.65 s.
                const double base = t < 1.0 ? 196.0 : 261.6;
                const bool silent = t > 0.55 && t < 0.65;
                for (int i = 0; i < block; ++i)
                {
                    const double ti = t + i / sr;
                    const double f = base * (1.0 + 0.012 * std::sin(2.0 * 3.14159265358979 * 5.5 * ti));
                    phase += 2.0 * 3.14159265358979 * f / sr;
                    const float v = silent ? 0.0f
                        : static_cast<float>(0.25 * std::sin(phase) + 0.12 * std::sin(2 * phase + 0.3) + 0.08 * std::sin(3 * phase));
                    buf.getWritePointer(0)[i] = v;
                    buf.getWritePointer(1)[i] = v;
                }
                // Flip the fast hop every 5 blocks (both directions, at arbitrary accumulator fill), and
                // Low Latency itself occasionally, as host automation could.
                if (b % 5 == 2)
                {
                    const bool on = !engine.getLowLatencyFastHop();
                    engine.setLowLatencyFastHop(on);
                    assert(engine.getLowLatencyFastHop() == on);
                }
                if (b % 61 == 30)
                    engine.setLowLatencyMode(!engine.getLowLatencyMode());
                engine.process(buf);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block; ++i)
                    {
                        const float y = buf.getReadPointer(ch)[i];
                        assert(std::isfinite(y) && std::abs(y) < 4.0f);
                    }
                processed += block;
            }
        }
    std::printf("fast_hop_toggle_regression: %lld samples per channel processed, output finite and bounded PASS\n", processed);
    return 0;
}
