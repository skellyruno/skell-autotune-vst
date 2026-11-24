#include "AutoTuneEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
#include <limits>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double f0 = 220.0;
constexpr int harmonics = 30;
struct Metrics {
    double firstFormant = 0, rms = 0, peak = 0, periodicity = 0;
    double detectedHz = 0, targetHz = 0;
    int delay = 0;
    bool finite = true;
};
std::vector<float> vowel(double sr)
{
    std::vector<float> input(static_cast<size_t>(sr * 1.2), 0.0f);
    for (size_t i = 0; i < input.size(); ++i) {
        const double phase = 2*pi*f0*i/sr;
        double sample = .11 * std::sin(phase);
        for (int h = 2; h <= harmonics; ++h) {
            const double hz = h * f0;
            const double envelope = .52 * std::exp(-.5*std::pow((hz-850)/160,2))
                 + .28 * std::exp(-.5*std::pow((hz-2300)/240,2))
                 + .04 * std::exp(-hz/2400);
            sample += .21 * envelope * std::sin(h*phase+.11*h*h);
        }
        input[i] = static_cast<float>(sample);
    }
    return input;
}
Metrics render(const std::vector<float>& input, double sr, int blockSize,
               bool low, bool preserve, float shift)
{
    humtune::AutoTuneEngine engine;
    engine.setVoiceRange(2);
    engine.setLowLatencyMode(low);
    engine.setRootNote(0);
    engine.setScaleType(0);
    engine.setRetuneSpeed(0);
    engine.setFormantPreserve(preserve);
    engine.setFormantShiftSemitones(shift);
    engine.prepare(sr, blockSize);
    std::vector<float> out(input.size(), 0.0f);
    for (int off = 0; off < static_cast<int>(input.size()); off += blockSize) {
        const int n = std::min(blockSize, static_cast<int>(input.size())-off);
        juce::AudioBuffer<float> block(1, n);
        std::copy_n(input.data()+off, n, block.getWritePointer(0));
        engine.process(block);
        std::copy_n(block.getReadPointer(0), n, out.data()+off);
    }
    Metrics m;
    m.detectedHz = engine.getDetectedHz();
    m.targetHz = engine.getTargetHz();
    m.delay = engine.getLatencySamples();
    for (float sample : out) {
        m.finite &= std::isfinite(sample);
        m.peak = std::max(m.peak, std::abs(static_cast<double>(sample)));
    }
    // Inspect only the settled last 0.4 seconds, away from acquisition.
    const int count = static_cast<int>(sr*.4);
    const int begin = static_cast<int>(out.size()) - count;
    double energy = 0, repeated = 0, reference = 0, cross = 0;
    const double lag = sr/f0;
    for (int i = begin; i < static_cast<int>(out.size()); ++i) {
        const double sample = out[static_cast<size_t>(i)];
        const double delayed = i-lag;
        const int j = static_cast<int>(delayed);
        const double a = out[static_cast<size_t>(j)];
        const double b = out[static_cast<size_t>(j+1)];
        const double prev = a + (b-a)*(delayed-j);
        energy += sample*sample;
        reference += sample*sample;
        repeated += prev*prev;
        cross += sample*prev;
    }
    m.rms = std::sqrt(energy/count);
    m.periodicity = cross/std::sqrt(std::max(reference*repeated, 1e-20));
    // First resonance centroid, excluding the deliberately strong F0.
    // A pure pitch shift cannot satisfy both spectral direction and the
    // unchanged fundamental/periodicity checks.
    double weighted = 0, power = 0;
    for (int h = 2; h <= 7; ++h) {
        double re = 0, im = 0;
        const double hz = h*f0;
        for (int k = 0; k < count; ++k) {
            const double w = .5-.5*std::cos(2*pi*k/count);
            const double phase = 2*pi*hz*k/sr;
            const double s = w*out[static_cast<size_t>(begin+k)];
            re += s*std::cos(phase); im -= s*std::sin(phase);
        }
        const double p = re*re+im*im;
        weighted += hz*p;
        power += p;
    }
    m.firstFormant = weighted/std::max(power,1e-20);
    return m;
}
}
int main()
{
    bool ok = true;
    // Standard and Low Latency, both preserve settings, at three sample
    // rates and three representative block sizes (including 1 sample).
    const std::array<std::pair<double,int>,3> configurations{{{44100,127},{48000,1},{96000,512}}};
    for (const auto& config : configurations) {
        const auto [sr, block] = config;
        const auto input = vowel(sr);
        for (bool low : {false,true}) for (bool preserve : {false,true}) {
            std::array<Metrics,3> result{};
            int i=0;
            for (float shift : {-4.0f,0.0f,4.0f}) {
                const auto m = render(input,sr,block,low,preserve,shift);
                result[static_cast<size_t>(i++)] = m;
                const bool pass = m.finite && m.rms > .02 && m.peak < 1.0
                    && m.periodicity > .90 && std::abs(m.detectedHz-f0) < 3.0
                    && std::abs(m.targetHz-f0) < 1.0;
                std::printf("formant sr=%.0f block=%d low=%d preserve=%d shift=%+.0f F1=%.2f Hz rms=%.5f peak=%.5f period=%.5f detected=%.3f target=%.3f latency=%d %s\n",
                    sr,block,low,preserve,shift,m.firstFormant,m.rms,m.peak,m.periodicity,m.detectedHz,m.targetHz,m.delay,pass?"PASS":"FAIL");
                ok &= pass;
            }
            const bool direction = result[0].firstFormant + 35.0 < result[1].firstFormant
                && result[1].firstFormant + 35.0 < result[2].firstFormant;
            const bool delay = result[0].delay == result[1].delay && result[1].delay == result[2].delay;
            std::printf("direction %.0f/%d low=%d preserve=%d: %.2f < %.2f < %.2f Hz; latency unchanged=%d %s\n",
                sr,block,low,preserve,result[0].firstFormant,result[1].firstFormant,
                result[2].firstFormant,delay,(direction&&delay)?"PASS":"FAIL");
            ok &= direction && delay;
        }
    }
    return ok ? 0 : 1;
}
