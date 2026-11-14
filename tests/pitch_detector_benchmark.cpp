#include "PitchDetector.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;

void benchmarkFloor(double sampleRate, double floorHz)
{
    constexpr double seconds = 5.0;
    constexpr int blockSize = 192; // about the detector's 4 ms hop at 48 kHz
    const int total = static_cast<int>(sampleRate * seconds);
    std::vector<float> input(static_cast<size_t>(total));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        input[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(phase));
        phase += 2.0 * kPi * 220.0 / sampleRate;
    }

    humtune::PitchDetector detector;
    detector.prepare(sampleRate, blockSize);
    detector.setMinFrequency(static_cast<float>(floorHz));
    float sink = 0.0f;

    const auto start = std::chrono::steady_clock::now();
    for (int offset = 0; offset < total; offset += blockSize)
    {
        const int n = std::min(blockSize, total - offset);
        detector.feedSamples(input.data() + offset, n);
        sink += detector.detectPitch();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    std::printf("sample-rate %.1f kHz, floor %.0f Hz: %.3f ms elapsed for %.1f s audio; latest %.2f Hz; sink %.1f\n",
                sampleRate / 1000.0, floorHz, elapsed, seconds, detector.getDetectedHz(), sink);
}
}

int main(int argc, char**)
{
    // Timing is a diagnostic, not a brittle CI threshold. Run on the same machine/compiler
    // when comparing commits. A single argument restricts the probe to the legacy 70 Hz
    // floor so the same source can be compiled against the prior detector header.
    if (argc > 1)
    {
        benchmarkFloor(48000.0, 70.0);
        return 0;
    }
    benchmarkFloor(48000.0, 70.0);
    benchmarkFloor(48000.0, 40.0);
    benchmarkFloor(44100.0, 40.0);
    benchmarkFloor(96000.0, 40.0);
    return 0;
}
