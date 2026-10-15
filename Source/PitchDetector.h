#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace humtune
{

// YIN pitch detector (CMND = Cumulative Mean Normalized Difference).
//
// Fixes compared with the first version:
//  * The analysis window now contains BOTH halves of each comparison. The old
//    code compared recent samples with samples from the "future", which in a
//    ring buffer is old, unrelated audio, so pitch was often wrong.
//  * The window is copied into a flat array first. The old inner loop did four
//    integer divisions (modulo) per sample, which made it far too slow for the
//    audio thread.
//  * Detection runs once per "hop" (about every 6 ms) instead of once per audio
//    block, and silence is treated as unvoiced.
class PitchDetector
{
public:
    static constexpr float kMinHz = 35.0f;     // preallocated analysis window supports E1-range bass
    static constexpr float kMaxHz = 1500.0f;   // highest pitch we track

    void prepare (double sampleRate, int /*blockSize*/)
    {
        // Keep the YIN analysis rate near 12 kHz at every host sample rate. Pitch analysis
        // does not need full-band audio; 12 kHz still gives about 8 samples per period at
        // the 1,500 Hz tracking ceiling, while cutting the O(maxTau * integLen) difference
        // search dramatically for low notes (especially the new 40 Hz Sub Bass range).
        // A low-pass FIR in feedSamples() protects the reduced analysis stream from aliasing.
        dec = std::max(1, static_cast<int>(std::lround(sampleRate / 12000.0)));
        sr  = sampleRate / static_cast<double>(dec);        // the rate the detector works at
        accCount = 0;
        aaHistory.fill (0.0f);
        aaWrite = 0;
        configureAntiAliasFilter();

        // Allocate once for the lowest pitch we will ever support
        const int allocTau = std::max(16, static_cast<int>(std::ceil(sr / kMinHz)));
        const int allocBuf = allocTau + allocTau + 2;

        ring.assign(static_cast<size_t>(allocBuf), 0.0f);
        lin.assign(static_cast<size_t>(allocBuf), 0.0f);
        diffBuf.assign(static_cast<size_t>(allocTau + 2), 0.0f);
        cmndBuf.assign(static_cast<size_t>(allocTau + 2), 0.0f);

        minTau  = std::max(2, static_cast<int>(std::floor(sr / kMaxHz)));
        hopSize = std::max(32, static_cast<int>(sr / 250.0));

        // prepare() may be called again by a DAW after a sample-rate change.
        // Force the first setMinFrequency() call to recalculate the window even if
        // the requested minimum frequency is unchanged from the previous prepare.
        bufLen = 0;
        freshSamples = 0;
        setMinFrequency(kMinHz);
    }

    // Change the lowest pitch that is tracked. A higher value means a shorter
    // analysis window (less delay). Does not allocate. Old ring contents are
    // invalidated logically; detection stays unvoiced until a full new window has
    // been written, avoiding a bulk ring clear during host automation.
    void setMinFrequency (float hz)
    {
        hz = juce::jlimit(kMinHz, 400.0f, hz);
        if (bufLen > 0 && minHz == hz)
            return;
        minHz = hz;

        const int allocTau = static_cast<int>(diffBuf.size()) - 2;
        maxTau   = juce::jlimit(16, allocTau, static_cast<int>(std::ceil(sr / hz)));
        integLen = maxTau;                       // samples compared per lag
        bufLen   = integLen + maxTau + 2;        // samples needed in total

        writePos = 0;
        freshSamples = 0;
        samplesSinceDetect = 0;
        // Range changes invalidate pitch history and decimator memory, but do not
        // clear the larger detector rings on the real-time thread.
        aaHistory.fill (0.0f);
        aaWrite = 0;
        accCount = 0;
        haveResult = false;
        resetMedian();

        detectedHz = 0.0f;
        confidence = 0.0f;
    }

    float getMinFrequency() const { return minHz; }

    // Detection runs once every hopSize samples (about 4 ms)
    int getHopSize() const { return hopSize * dec; }

    // Feed samples into the ring buffer (at the detector's own, possibly reduced, rate).
    void feedSamples (const float* data, int numSamples)
    {
        if (ring.empty())
            return;

        int pushed = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            // Keep the FIR history at the original sample rate. Only evaluate its
            // output on decimation boundaries, so the extra filtering work scales
            // with the ~12 kHz detector rate rather than the host sample rate.
            aaHistory[static_cast<size_t>(aaWrite)] = data[i];
            if (++aaWrite >= kDecimatorTaps)
                aaWrite = 0;

            if (++accCount < dec)
                continue;

            accCount = 0;
            float filtered = 0.0f;
            int historyPos = aaWrite; // oldest sample after the just-written value
            for (int tap = 0; tap < kDecimatorTaps; ++tap)
            {
                filtered += decimatorCoeffs[static_cast<size_t>(tap)]
                          * aaHistory[static_cast<size_t>(historyPos)];
                if (++historyPos >= kDecimatorTaps)
                    historyPos = 0;
            }
            ring[static_cast<size_t>(writePos)] = filtered;
            if (++writePos >= bufLen)
                writePos = 0;
            if (freshSamples < bufLen)
                ++freshSamples;
            ++pushed;
        }

        samplesSinceDetect = std::min(samplesSinceDetect + pushed, 1 << 24);
    }

    // Returns the detected fundamental in Hz (0 if unvoiced). Between hops it
    // simply returns the previous result, which keeps the CPU cost low.
    float detectPitch()
    {
        if (ring.empty())
            return 0.0f;

        if (freshSamples < bufLen)
        {
            samplesSinceDetect = 0;
            return setUnvoiced();
        }

        if (haveResult && samplesSinceDetect < hopSize)
            return detectedHz;

        samplesSinceDetect = 0;
        haveResult = true;

        // Copy the ring into a flat array, oldest sample first
        const int tail = bufLen - writePos;
        std::memcpy(lin.data(), ring.data() + writePos, static_cast<size_t>(tail) * sizeof(float));
        if (writePos > 0)
            std::memcpy(lin.data() + tail, ring.data(), static_cast<size_t>(writePos) * sizeof(float));

        // The analysis uses the NEWEST `integLen` samples (b) and the same number
        // of samples one lag earlier (a), so the result describes "now", not the past.
        const int newest = bufLen - integLen;

        // Silence gate (about -70 dB): nothing to detect
        {
            float energy = 0.0f;
            for (int j = 0; j < integLen; ++j)
                energy += lin[static_cast<size_t>(newest + j)] * lin[static_cast<size_t>(newest + j)];
            if (energy < static_cast<float>(integLen) * 1.0e-7f)
                return setUnvoiced();
        }

        // Step 1: difference function
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            const float* a = lin.data() + (newest - tau);
            const float* b = lin.data() + newest;
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            int j = 0;
            for (; j + 4 <= integLen; j += 4)
            {
                const float d0 = a[j]     - b[j];
                const float d1 = a[j + 1] - b[j + 1];
                const float d2 = a[j + 2] - b[j + 2];
                const float d3 = a[j + 3] - b[j + 3];
                s0 += d0 * d0;  s1 += d1 * d1;  s2 += d2 * d2;  s3 += d3 * d3;
            }
            for (; j < integLen; ++j)
            {
                const float d = a[j] - b[j];
                s0 += d * d;
            }
            diffBuf[static_cast<size_t>(tau)] = (s0 + s1) + (s2 + s3);
        }

        // Step 2: cumulative mean normalized difference
        cmndBuf[0] = 1.0f;
        float runningSum = 0.0f;
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            runningSum += diffBuf[static_cast<size_t>(tau)];
            cmndBuf[static_cast<size_t>(tau)] =
                diffBuf[static_cast<size_t>(tau)] * static_cast<float>(tau)
                / std::max(runningSum, 1.0e-12f);
        }

        // Step 3: absolute threshold search
        const int lastTau = maxTau - 1;      // keep tau+1 in range
        int tauEst = -1;

        for (int tau = minTau; tau <= lastTau; ++tau)
        {
            if (cmndBuf[static_cast<size_t>(tau)] < yinThreshold)
            {
                while (tau + 1 <= lastTau
                    && cmndBuf[static_cast<size_t>(tau + 1)] < cmndBuf[static_cast<size_t>(tau)])
                    ++tau;
                tauEst = tau;
                break;
            }
        }

        // No dip below the threshold: take the global minimum if it is decent
        if (tauEst < 0)
        {
            float best = 999.0f;
            for (int tau = minTau; tau <= lastTau; ++tau)
            {
                if (cmndBuf[static_cast<size_t>(tau)] < best)
                {
                    best = cmndBuf[static_cast<size_t>(tau)];
                    tauEst = tau;
                }
            }
            // A weak match is more likely an octave error than a real pitch:
            // better to leave the audio alone than to "correct" the wrong note.
            if (best > 0.3f)
                return setUnvoiced();
        }

        if (tauEst < 1)
            return setUnvoiced();

        // Step 4: parabolic interpolation for sub-sample accuracy
        float betterTau = static_cast<float>(tauEst);
        if (tauEst > 1 && tauEst < maxTau)
        {
            // Interpolate on the raw difference curve: it is much closer to a
            // parabola near its minimum than the normalized (CMND) curve.
            const float s0 = diffBuf[static_cast<size_t>(tauEst - 1)];
            const float s1 = diffBuf[static_cast<size_t>(tauEst)];
            const float s2 = diffBuf[static_cast<size_t>(tauEst + 1)];
            const float denom = 2.0f * (s0 - 2.0f * s1 + s2);   // vertex of the parabola through the 3 points
            if (std::abs(denom) > 1.0e-9f)
                betterTau += juce::jlimit(-1.0f, 1.0f, (s0 - s2) / denom);
        }

        const float rawHz = static_cast<float>(sr) / betterTau;
        confidence = juce::jlimit(0.0f, 1.0f, 1.0f - cmndBuf[static_cast<size_t>(tauEst)]);

        // Step 5: median filter for stability
        medianHistory[static_cast<size_t>(medianIdx)] = rawHz;
        medianIdx = (medianIdx + 1) % kMedianSize;
        medianCount = std::min(medianCount + 1, kMedianSize);

        detectedHz = medianFiltered();
        return detectedHz;
    }

    float getDetectedHz()  const { return detectedHz; }
    float getConfidence()  const { return confidence; }

    void setThreshold (float t) { yinThreshold = t; }

private:
    static constexpr int kMedianSize = 3;
    static constexpr int kDecimatorTaps = 31;
    static constexpr double kPi = 3.14159265358979323846;

    void configureAntiAliasFilter()
    {
        decimatorCoeffs.fill (0.0f);
        if (dec <= 1)
        {
            decimatorCoeffs[static_cast<size_t>(kDecimatorTaps / 2)] = 1.0f;
            return; // no downsampling means no anti-alias filter is needed
        }

        // Blackman-windowed low-pass FIR. The cutoff is 80% of the decimated
        // Nyquist frequency to leave a practical transition band; 31 taps are
        // evaluated only once per detector sample.
        const double cutoff = 0.40 / static_cast<double>(dec); // cycles / input sample
        const int centre = kDecimatorTaps / 2;
        double sum = 0.0;
        for (int n = 0; n < kDecimatorTaps; ++n)
        {
            const int m = n - centre;
            const double ideal = (m == 0) ? 2.0 * cutoff
                : std::sin (2.0 * kPi * cutoff * static_cast<double>(m))
                  / (kPi * static_cast<double>(m));
            const double phase = 2.0 * kPi * static_cast<double>(n)
                               / static_cast<double>(kDecimatorTaps - 1);
            const double window = 0.42 - 0.5 * std::cos (phase)
                                        + 0.08 * std::cos (2.0 * phase);
            const double coefficient = ideal * window;
            decimatorCoeffs[static_cast<size_t>(n)] = static_cast<float>(coefficient);
            sum += coefficient;
        }
        const float inverseSum = static_cast<float>(1.0 / std::max (sum, 1.0e-12));
        for (auto& coefficient : decimatorCoeffs)
            coefficient *= inverseSum;
    }

    float setUnvoiced()
    {
        detectedHz = 0.0f;
        confidence = 0.0f;
        resetMedian();
        return 0.0f;
    }

    void resetMedian()
    {
        medianHistory.fill(0.0f);
        medianIdx = 0;
        medianCount = 0;
    }

    float medianFiltered() const
    {
        const int count = std::min(medianCount, kMedianSize);
        float tmp[kMedianSize];
        for (int i = 0; i < count; ++i)
            tmp[i] = medianHistory[static_cast<size_t>(i)];
        for (int i = 1; i < count; ++i)            // tiny insertion sort
        {
            const float v = tmp[i];
            int k = i - 1;
            while (k >= 0 && tmp[k] > v) { tmp[k + 1] = tmp[k]; --k; }
            tmp[k + 1] = v;
        }
        return count > 0 ? tmp[count / 2] : 0.0f;
    }

    double sr = 44100.0;      // rate the detector works at (host rate divided by dec)
    int dec = 1;              // decimation factor
    std::array<float, kDecimatorTaps> aaHistory {};
    std::array<float, kDecimatorTaps> decimatorCoeffs {};
    int aaWrite = 0;
    int accCount = 0;
    int maxTau = 0, minTau = 0, integLen = 0, bufLen = 0, hopSize = 256;
    float minHz = kMinHz;
    int writePos = 0;
    int freshSamples = 0;
    int samplesSinceDetect = 0;
    bool haveResult = false;

    std::vector<float> ring, lin, diffBuf, cmndBuf;

    float yinThreshold = 0.15f;
    float detectedHz = 0.0f;
    float confidence = 0.0f;

    std::array<float, kMedianSize> medianHistory {};
    int medianIdx = 0;
    int medianCount = 0;
};

} // namespace humtune
