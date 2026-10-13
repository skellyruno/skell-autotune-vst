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
//  * Detection runs on a short, overlapping hop (about every 5 ms) instead of once per
//    host audio block, and silence is treated as unvoiced.
class PitchDetector
{
public:
    static constexpr float kMinHz = 70.0f;     // lowest pitch we can ever track (buffers are sized for it)
    static constexpr float kMaxHz = 1500.0f;   // highest pitch we track

    void prepare (double sampleRate, int /*blockSize*/)
    {
        // Run the detector near 24 kHz regardless of host rate. A box-filtered
        // decimation keeps the 70–1500 Hz fundamental range comfortably below
        // Nyquist while cutting the quadratic YIN work by roughly 4x at 48 kHz.
        // The shifter itself still runs at the full host sample rate.
        const double hostRate = std::max(1.0, sampleRate);
        dec = std::max(1, static_cast<int>(std::lround(hostRate / 24000.0)));
        sr  = hostRate / static_cast<double>(dec);        // the rate the detector works at
        accum = 0.0f;
        accCount = 0;
        previousInput = previousHighPass = 0.0f;
        // Detection-only high-pass: reject DC/handling rumble without putting a filter
        // into the audible path. 45 Hz keeps the bottom of the 70-100 Hz range usable.
        highPassCoeff = static_cast<float>(std::exp(-2.0 * juce::MathConstants<double>::pi
                                                    * 45.0 / hostRate));

        // The adaptive YIN window can grow to 1.5 periods while retaining enough
        // history for the largest legal lag. Storage is fixed here: the audio thread
        // never allocates when the previous pitch changes the analysis window.
        const int allocTau = std::max(16, static_cast<int>(std::ceil(sr / kMinHz)));
        const int allocBuf = 3 * allocTau + 8;

        ring.assign(static_cast<size_t>(allocBuf), 0.0f);
        lin.assign(static_cast<size_t>(allocBuf), 0.0f);
        diffBuf.assign(static_cast<size_t>(allocTau + 2), 0.0f);
        cmndBuf.assign(static_cast<size_t>(allocTau + 2), 0.0f);

        minTau  = std::max(2, static_cast<int>(std::floor(sr / kMaxHz)));
        hopSize = std::max(32, static_cast<int>(std::lround(sr / 200.0)));

        setMinFrequency(kMinHz);
    }

    // Change the lowest pitch that is tracked. A higher value means a shorter
    // analysis window (less delay). Does not allocate, so it is safe to call
    // from the audio thread. Clears the detector's history.
    void setMinFrequency (float hz)
    {
        hz = juce::jlimit(kMinHz, 400.0f, hz);
        minHz = hz;

        const int allocTau = static_cast<int>(diffBuf.size()) - 2;
        maxTau   = juce::jlimit(16, allocTau, static_cast<int>(std::ceil(sr / hz)));
        integLen = maxTau;                       // samples compared per lag
        bufLen   = integLen + maxTau + 2;        // samples needed in total

        std::fill(ring.begin(), ring.end(), 0.0f);
        writePos = 0;
        samplesSinceDetect = 0;
        haveResult = false;
        resetMedian();

        detectedHz = 0.0f;
        confidence = 0.0f;
    }

    float getMinFrequency() const { return minHz; }

    // Detection runs once every hopSize samples (about 5 ms)
    int getHopSize() const { return hopSize * dec; }

    // Feed samples into the ring buffer (at the detector's own, possibly reduced, rate).
    void feedSamples (const float* data, int numSamples)
    {
        if (ring.empty())
            return;

        int pushed = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            // Filter only the pitch-analysis tap. Audio passed to the shifter remains
            // untouched, so the conditioning cannot change tone or add audible phase shift.
            const float x = data[i];
            const float filtered = highPassCoeff * (previousHighPass + x - previousInput);
            previousInput = x;
            previousHighPass = filtered;
            accum += filtered;
            if (++accCount < dec)
                continue;

            ring[static_cast<size_t>(writePos)] = accum / static_cast<float>(dec);
            accum = 0.0f;
            accCount = 0;
            if (++writePos >= static_cast<int>(ring.size()))
                writePos = 0;
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

        if (haveResult && samplesSinceDetect < hopSize)
            return detectedHz;

        samplesSinceDetect = 0;
        haveResult = true;

        // Use a pitch-dependent analysis window. Once a usable f0 is known, analyse
        // about 1.5 cycles instead of always paying for the lowest configured note.
        // The first estimate stays conservative so low bass fundamentals remain findable.
        const int ringCapacity = static_cast<int>(ring.size());
        const int maxAnalysis = std::min(ringCapacity - maxTau - 2,
                                         static_cast<int>(std::ceil(1.5 * maxTau)));
        const int minAnalysis = std::min(maxAnalysis, std::max(48, 2 * minTau));
        int requestedAnalysis = maxTau;
        if (detectedHz >= minHz && detectedHz <= kMaxHz)
        {
            const double previousTau = sr / static_cast<double>(detectedHz);
            requestedAnalysis = static_cast<int>(std::ceil(1.5 * previousTau));
        }
        integLen = juce::jlimit(minAnalysis, maxAnalysis, requestedAnalysis);
        bufLen = integLen + maxTau + 2;

        // Copy the newest `bufLen` samples from the circular store to a contiguous
        // analysis buffer, oldest first. The physical ring has fixed capacity while
        // `bufLen` changes, so no data movement/reset is required when f0 changes.
        int start = writePos - bufLen;
        if (start < 0)
            start += ringCapacity;
        const int firstCount = std::min(bufLen, ringCapacity - start);
        std::memcpy(lin.data(), ring.data() + start, static_cast<size_t>(firstCount) * sizeof(float));
        if (firstCount < bufLen)
            std::memcpy(lin.data() + firstCount, ring.data(),
                        static_cast<size_t>(bufLen - firstCount) * sizeof(float));

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
    int getAnalysisWindowSamples() const { return integLen; }
    double getAnalysisWindowMilliseconds() const { return 1000.0 * static_cast<double>(integLen) / sr; }

    void setThreshold (float t) { yinThreshold = t; }

private:
    static constexpr int kMedianSize = 3;

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
    float accum = 0.0f;       // running sum of the current group of input samples
    float previousInput = 0.0f;
    float previousHighPass = 0.0f;
    float highPassCoeff = 0.0f;
    int accCount = 0;
    int maxTau = 0, minTau = 0, integLen = 0, bufLen = 0, hopSize = 256;
    float minHz = kMinHz;
    int writePos = 0;
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
