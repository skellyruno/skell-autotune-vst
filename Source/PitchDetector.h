#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define SKELLYTUNE_DETECTOR_SSE2 1
#else
#define SKELLYTUNE_DETECTOR_SSE2 0
#endif

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
        updateHopSize();

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

    // Re-arm an existing detector after an input-time gap. Only small FIR and
    // median state is cleared; ring history is invalidated by freshSamples.
    void resetHistory() noexcept {
        writePos=0;freshSamples=0;samplesSinceDetect=0;
        aaHistory.fill(0.0f);aaWrite=0;accCount=0;
        haveResult=false;analysisSampleCounter=0;jumpPending=false;resetMedian();
        detectedHz=confidence=0.0f;
    }
    float getMinFrequency() const { return minHz; }

    // EXPERIMENTAL, Low Latency only, default off (SKELLYTUNE_LL_FAST_HOP). Re-measure every ~2 ms
    // instead of ~4 ms so estimates are fresher at the moment a note changes. The engine's
    // 4 ms-tuned timing (onset energy, note-transition motion, unvoiced counts) is expressed in
    // time, so it is unchanged at 4 ms; see FINDINGS for the known alias-rejection caveat.
    void setFastHop (bool on) noexcept
    {
        if (fastHop == on)
            return;
        fastHop = on;
        updateHopSize();
    }
    bool getFastHop() const noexcept { return fastHop; }

    // Low Latency only. Once a voiced track is established (the first three estimates after
    // silence still go through the median, where octave-confused onsets are most likely), report
    // each estimate as soon as it is measured instead of through the three-frame median, which
    // adds one hop (about 4 ms) to every estimate and so delays every note change. The median's job is outlier rejection, so a sudden jump of more than
    // kFastJumpCents (an octave error, a consonant) is held for one hop and accepted only if the
    // next estimate is still away from the previous value. The held value keeps ageing, and
    // both age getters report that, so the engine's timing compensation stays exact.
    void setFastResponse (bool on) noexcept
    {
        if (fastResponse == on)
            return;
        fastResponse = on;
        updateHopSize();
        jumpPending = false;
        jumpHeldHops = 0;
        resetMedian();
        detectedHz = 0.0f;
        confidence = 0.0f;
        haveResult = false;
    }

    // Detection runs once every hopSize samples (about 4 ms).
    int getHopSize() const { return hopSize * dec; }

    // Approximate age of the current YIN estimate. The estimate correlates two
    // windows separated by one candidate period; their combined midpoint trails
    // the newest sample by roughly half the analysis window plus half a period.
    // The three-frame median adds one hop of group delay for a smoothly moving pitch.
    // Returning this timing metadata lets the engine compensate for detector lag
    // without adding a separate audio buffer or increasing reported plugin latency.
    double getPitchEstimateAgeSeconds (float frequencyHz) const noexcept
    {
        if (sr <= 0.0 || hopSize <= 0 || integLen <= 0
            || !std::isfinite(frequencyHz) || frequencyHz <= 0.0f)
            return 0.0;

        const double tau = std::clamp(sr / static_cast<double>(frequencyHz),
                                       static_cast<double>(minTau),
                                       static_cast<double>(maxTau));
        if (fastResponse)
        {
            const double half = selectedWindowHalf > 0.0 ? selectedWindowHalf : 0.5 * static_cast<double>(integLen - 1);
            const double held = analysisSampleCounter >= selectedMedianSample
                ? static_cast<double>(analysisSampleCounter - selectedMedianSample) : 0.0;
            return (half + 0.5 * tau + held) / sr;
        }
        const double pairedWindowMidpoint = 0.5 * (static_cast<double>(integLen - 1) + tau);
        // During the first two voiced frames the median filter is not yet full,
        // so do not compensate it as if it already had its full one-hop delay.
        const double medianGroupDelay = medianCount >= kMedianSize ? static_cast<double>(hopSize)
            : (medianCount == 2 ? 0.5 * static_cast<double>(hopSize) : 0.0);
        return (pairedWindowMidpoint + medianGroupDelay) / sr;
    }

    // Additional observation tags for epoch-evidence qualification only.
    // The original controller getter above retains its exact legacy timing.
    double getTaggedPitchEstimateAgeSeconds (float frequencyHz) const noexcept
    {
        if (sr <= 0.0 || hopSize <= 0 || integLen <= 0
            || !std::isfinite(frequencyHz) || frequencyHz <= 0.0f)
            return 0.0;

        const double tau = std::clamp(sr / static_cast<double>(frequencyHz),
                                       static_cast<double>(minTau),
                                       static_cast<double>(maxTau));
        const double pairedWindowMidpoint = (medianCount>0 ? selectedWindowHalf : .5*(integLen-1)) + .5*tau;
        // During the first two voiced frames the median filter is not yet full,
        // so do not compensate it as if it already had its full one-hop delay.
        const double medianGroupDelay = medianCount>0 && analysisSampleCounter>=selectedMedianSample
            ? static_cast<double>(analysisSampleCounter-selectedMedianSample) : 0.0;
        return (pairedWindowMidpoint + medianGroupDelay) / sr
             + (accCount+.5*(kDecimatorTaps-1))/(sr*dec);
    }

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

        analysisSampleCounter+=static_cast<std::uint64_t>(pushed);
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

        // At the fast hop the first estimates after silence (median still filling) keep the original
        // 4 ms cadence, so the octave-confusion protection spans the same time as before.
        if (fastResponse && onsetStride > 1 && medianCount > 0 && medianCount < kMedianSize
            && (++onsetPhase % onsetStride) != 0)
        {
            samplesSinceDetect = 0;
            return detectedHz;
        }

        samplesSinceDetect = 0;
        haveResult = true;

        // Copy the ring into a flat array, oldest sample first
        const int tail = bufLen - writePos;
        std::memcpy(lin.data(), ring.data() + writePos, static_cast<size_t>(tail) * sizeof(float));
        if (writePos > 0)
            std::memcpy(lin.data() + tail, ring.data(), static_cast<size_t>(writePos) * sizeof(float));

        // Low Latency, established track: compare only about one period of the newest audio
        // instead of the full longest-period window, so the estimate describes fresher audio.
        bool fullWindowRetry = false;
    retryWithFullWindow:
        int integ = integLen;
        if (fastResponse && !fullWindowRetry && medianCount >= kMedianSize && detectedHz > 0.0f)
            integ = std::clamp(static_cast<int>(std::lround(kFastWindowPeriods * sr / detectedHz)),
                               std::max(32, integLen / 3), integLen);
        // The analysis uses the NEWEST `integ` samples (b) and the same number
        // of samples one lag earlier (a), so the result describes "now", not the past.
        const int newest = bufLen - integ;

        // Silence gate (about -70 dB): nothing to detect
        {
            float energy = 0.0f;
            for (int j = 0; j < integ; ++j)
                energy += lin[static_cast<size_t>(newest + j)] * lin[static_cast<size_t>(newest + j)];
            if (energy < static_cast<float>(integ) * 1.0e-7f)
                return setUnvoiced();
        }

        // Step 1: difference function
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            const float* a = lin.data() + (newest - tau);
            const float* b = lin.data() + newest;
            diffBuf[static_cast<size_t>(tau)] = squaredDifference(a, b, integ);
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
        bool rescuedThisHop = false;

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
            {
                // Continuity rescue (Standard). During a glide or an irregular cycle the period
                // changes inside the analysis window and the best dip rises above 0.3 although the
                // voice is still periodic. Inside an established track, look only near the previous
                // period and accept a moderate dip for at most a couple of hops instead of dropping
                // the note and passing the audio through uncorrected. Below this the signal shows no
                // periodicity at all (breath, consonant), where the dry audio is the right output.
                int rescued = -1;
                const int maxRescueHops = std::max(1, static_cast<int>(std::lround(kRescueSeconds * sr / hopSize)));
                if (!fastResponse && rescueHops < maxRescueHops && detectedHz > 0.0f && medianCount >= kMedianSize)
                {
                    const double prevTau = sr / detectedHz;
                    const int lo = std::max(minTau, static_cast<int>(std::floor(prevTau * 0.75)));
                    const int hi = std::min(lastTau, static_cast<int>(std::ceil(prevTau * 1.33)));
                    float rb = 999.0f;
                    for (int tau = lo; tau <= hi; ++tau)
                        if (cmndBuf[static_cast<size_t>(tau)] < rb) { rb = cmndBuf[static_cast<size_t>(tau)]; rescued = tau; }
                    if (!(rb <= kRescueCmnd))
                        rescued = -1;
                }
                if (rescued < 0)
                {
                    // The short window can lose a pitch the full window still finds (rough or
                    // quiet cycles): retry once with the full window before giving up.
                    if (fastResponse && !fullWindowRetry && integ < integLen)
                    {
                        fullWindowRetry = true;
                        goto retryWithFullWindow;
                    }
                    return setUnvoiced();
                }
                tauEst = rescued;
                rescuedThisHop = true;
            }
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

        // Compare refined periods on the same recorded waveform. Integer-lag
        // CMND values alone favor some subharmonics due to fractional-lag bias.
        float fractionalConfidence=0;
        double observationWindowHalf=.5*(integ-1);
        if (cmndBuf[static_cast<size_t>(tauEst)]>.01f && cmndBuf[static_cast<size_t>(tauEst)]<.20f)
        {
            const double span=std::clamp(4.0*betterTau/sr,.006,.012);
            struct Evidence {double lag;float match;};
            const auto refine=[&](double lag) noexcept {
                std::array<float,5> score{};
                int best=0;
                for(int i=0;i<5;++i) {
                    const double value=lag+(i-2)*.25;
                    score[static_cast<size_t>(i)]=getRecentPeriodCorrelation(sr/value,span);
                    if(score[static_cast<size_t>(i)]>score[static_cast<size_t>(best)])best=i;
                }
                Evidence result{lag+(best-2)*.25,score[static_cast<size_t>(best)]};
                if(best>0 && best<4) {
                    const double lo=score[static_cast<size_t>(best-1)],hi=score[static_cast<size_t>(best+1)],mid=score[static_cast<size_t>(best)];
                    const double curve=lo-2*mid+hi;
                    if(curve<-1e-9) {
                        const double offset=.25*std::clamp(.5*(lo-hi)/curve,-.5,.5);
                        const double position=result.lag+offset;
                        const float fit=getRecentPeriodCorrelation(sr/position,span);
                        if(fit>result.match)result={position,fit};
                    }
                }
                return result;
            };
            const auto shortest=refine(betterTau);
            auto best=shortest;
            if(shortest.match>=0 && shortest.match<.98f) {
                const int start=std::max(tauEst+1,static_cast<int>(std::ceil(1.2*betterTau)));
                const int end=std::min(lastTau-1,static_cast<int>(4*betterTau+1));
                for(int lag=start;lag<=end;++lag) {
                    const float center=cmndBuf[static_cast<size_t>(lag)];
                    if(center>.30f || center>cmndBuf[static_cast<size_t>(lag-1)] || center>=cmndBuf[static_cast<size_t>(lag+1)])continue;
                    const double lo=diffBuf[static_cast<size_t>(lag-1)],mid=diffBuf[static_cast<size_t>(lag)],hi=diffBuf[static_cast<size_t>(lag+1)];
                    const double curve=2*(lo-2*mid+hi);
                    const double seed=lag+(std::abs(curve)>1e-9 ? std::clamp((lo-hi)/curve,-1.0,1.0) : 0.0);
                    const auto longer=refine(seed);
                    const float improvement=longer.match-shortest.match;
                    const double candidateHz=sr/longer.lag;
                    const bool continuous=detectedHz>0 &&
                        std::abs(1200*std::log2(candidateHz/detectedHz))<175;
                    if((longer.match>.995f && improvement>.02f) ||
                       (continuous && longer.match>.97f && improvement>.06f &&
                        getRecentPeriodCorrelation(sr/shortest.lag,.002)<.93f)) {best=longer;break;}
                }
            }
            if(best.lag>shortest.lag*1.15) {
                fractionalConfidence=best.match;
                observationWindowHalf=3+.5*(std::clamp(static_cast<int>(span*sr),16,integLen)-1);
                betterTau=static_cast<float>(best.lag);
                tauEst=std::clamp(static_cast<int>(std::lround(best.lag)),minTau,lastTau);
            }
        }
        if (rescuedThisHop) ++rescueHops; else rescueHops = 0;
        const float rawHz = static_cast<float>(sr) / betterTau;
        confidence = std::max(fractionalConfidence,juce::jlimit(0.0f, 1.0f, 1.0f - cmndBuf[static_cast<size_t>(tauEst)]));

        if (fastResponse && medianCount >= kMedianSize)
        {
            const bool jump = detectedHz > 0.0f
                && std::abs(1200.0f * std::log2(rawHz / detectedHz)) > kFastJumpCents;
            // A large jump is held for one 4 ms reference interval (one hop at the normal rate,
            // onsetStride hops at the fast rate) and accepted only if it persists.
            if (jump && jumpHeldHops < onsetStride)
            {
                ++jumpHeldHops;
                return detectedHz;
            }
            jumpHeldHops = 0;
            jumpPending = false;
            medianHistory.fill(rawHz);
            medianSampleTimes.fill(analysisSampleCounter);
            medianWindowHalves.fill(observationWindowHalf);
            medianCount = kMedianSize;
            selectedMedianSample = analysisSampleCounter;
            selectedWindowHalf = observationWindowHalf;
            detectedHz = rawHz;
            return detectedHz;
        }

        // Step 5: median filter for stability
        medianHistory[static_cast<size_t>(medianIdx)] = rawHz;
        medianSampleTimes[static_cast<size_t>(medianIdx)]=analysisSampleCounter;
        medianWindowHalves[static_cast<size_t>(medianIdx)]=observationWindowHalf;
        medianIdx = (medianIdx + 1) % kMedianSize;
        medianCount = std::min(medianCount + 1, kMedianSize);

        detectedHz = medianFiltered();
        return detectedHz;
    }

    // Independent recent-waveform evidence for a proposed pitch. detectPitch()
    // has already flattened this recorded history; use the actual detector mono
    // signal so stereo source selection and cancellation handling stay consistent.
    float getRecentPeriodCorrelation (double hz, double spanSeconds) const noexcept
    {
        if (!haveResult || freshSamples < bufLen || hz <= 0 || sr <= 0)
            return -1;
        const double lag = sr / hz;
        const int span = std::clamp (static_cast<int> (spanSeconds * sr), 16, integLen);
        const int end = bufLen - 3, begin = end - span;
        if (begin - lag < 1 || lag < 2)
            return -1;
        double xy = 0, xx = 0, yy = 0;
        for (int i = begin; i < end; ++i)
        {
            const double p = i - lag;
            const int j = static_cast<int> (p);
            const double f = p - j;
            const double a = lin[static_cast<size_t> (j - 1)], b = lin[static_cast<size_t> (j)];
            const double c = lin[static_cast<size_t> (j + 1)], d = lin[static_cast<size_t> (j + 2)];
            const double y =
                b + .5 * f * (c - a + f * (2 * a - 5 * b + 4 * c - d + f * (3 * (b - c) + d - a)));
            const double x = lin[static_cast<size_t> (i)];
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        return xx > 1e-9 && yy > 1e-9 ? static_cast<float> (xy / std::sqrt (xx * yy)) : -1;
    }

    float getDetectedHz()  const { return detectedHz; }
    float getConfidence()  const { return confidence; }

    void setThreshold (float t) { yinThreshold = t; }

private:
    friend struct PitchDetectorDifferenceOracle;
    // Four independent lanes preserve the scalar accumulation/reduction order.
    // Unaligned loads are intentional: lag offsets need not be 16-byte aligned.
    // No FMA/reassociation, allocation, or extra detector observation delay.
    static float squaredDifference(const float* a, const float* b, int count) noexcept
    {
        float s0=0, s1=0, s2=0, s3=0;
        int j=0;
#if SKELLYTUNE_DETECTOR_SSE2
        __m128 accumulated=_mm_setzero_ps();
        for (; j+4<=count; j+=4) {
            const __m128 delta=_mm_sub_ps(_mm_loadu_ps(a+j),_mm_loadu_ps(b+j));
            accumulated=_mm_add_ps(accumulated,_mm_mul_ps(delta,delta));
        }
        float lanes[4]; _mm_storeu_ps(lanes,accumulated);
        s0=lanes[0]; s1=lanes[1]; s2=lanes[2]; s3=lanes[3];
#else
        for (; j+4<=count; j+=4) {
            const float d0=a[j]-b[j], d1=a[j+1]-b[j+1], d2=a[j+2]-b[j+2], d3=a[j+3]-b[j+3];
            s0+=d0*d0; s1+=d1*d1; s2+=d2*d2; s3+=d3*d3;
        }
#endif
        for (; j<count; ++j) {const float d=a[j]-b[j];s0+=d*d;}
        return (s0+s1)+(s2+s3);
    }

    static constexpr int kMedianSize = 3;
    static constexpr float kFastJumpCents = 400.0f;
    static constexpr float kFastWindowPeriods = 1.2f;   // Low Latency analysis window, in periods
    bool fastResponse = false, jumpPending = false;
#ifndef SKELLYTUNE_LL_FAST_HOP
#define SKELLYTUNE_LL_FAST_HOP 0
#endif
    bool fastHop = SKELLYTUNE_LL_FAST_HOP != 0;
    static constexpr double kFastHopHz = 500.0;          // fast hop: ~2 ms
    static constexpr int kFastMinHop = 16;
    int jumpHeldHops = 0;
    int onsetStride = 1, onsetPhase = 0;                 // first estimates after silence keep the 4 ms cadence
    void updateHopSize() noexcept
    {
        if (sr <= 0.0)
            return;
        hopSize = (fastResponse && fastHop) ? std::max(kFastMinHop, static_cast<int>(sr / kFastHopHz))
                                            : std::max(32, static_cast<int>(sr / 250.0));
        onsetStride = std::max(1, static_cast<int>(std::lround(0.004 * sr / hopSize)));
    }
    int rescueHops = 0;
    static constexpr float kRescueCmnd = 0.55f;       // loosest dip accepted by the continuity rescue
    static constexpr double kRescueSeconds = 0.008;   // ...for at most this long
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
        jumpPending = false;
        jumpHeldHops = 0;
        rescueHops = 0;
        detectedHz = 0.0f;
        confidence = 0.0f;
        resetMedian();
        return 0.0f;
    }

    void resetMedian()
    {
        medianHistory.fill(0.0f);
        medianSampleTimes.fill(analysisSampleCounter);medianWindowHalves.fill(0);
        selectedMedianSample=analysisSampleCounter;selectedWindowHalf=0;
        medianIdx = 0;
        onsetPhase = 0;
        medianCount = 0;
    }

    float medianFiltered()
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
        const float result=count>0 ? tmp[count/2] : 0;
        selectedMedianSample=0;
        for(int i=0;i<count;++i)if(medianHistory[static_cast<size_t>(i)]==result && medianSampleTimes[static_cast<size_t>(i)]>=selectedMedianSample) {
            selectedMedianSample=medianSampleTimes[static_cast<size_t>(i)];selectedWindowHalf=medianWindowHalves[static_cast<size_t>(i)];
        }
        return result;
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
    std::array<std::uint64_t,kMedianSize> medianSampleTimes{};
    std::array<double,kMedianSize> medianWindowHalves{};
    std::uint64_t analysisSampleCounter=0,selectedMedianSample=0;
    double selectedWindowHalf=0;
    int medianIdx = 0;
    int medianCount = 0;
};

} // namespace humtune
